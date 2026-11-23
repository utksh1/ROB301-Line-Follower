/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "debug_uart.h"
#include <stdio.h>
#include <stdlib.h>
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum { STOPPED, FOLLOW, REVERSE } RobotState;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DEFAULT_BASE_SPEED  70
#define MIN_BASE_SPEED      30
#define MAX_BASE_SPEED      90

#define MIN_PWM             55
#define MAX_PWM             90

// ── Default PID gains (tunable live from terminal) ───────────────────────────
#define KP_DEFAULT          2.2f
#define KI_DEFAULT          0.0f  // Pure PD eliminates phase-lag hunting oscillation
#define KD_DEFAULT          16.0f

#define MAX_CORRECTION      25
#define INTEGRAL_LIMIT      20.0f  // anti-windup clamp on the I-accumulator

// ── Anti-wobble tuning ───────────────────────────────────────────────────────
// Deadband only covers "perfectly centred" (sensors 3+4 both on → E=0).
// The old 0.55 deadband also swallowed E=±0.5 (one centre sensor), letting the
// robot drift a full sensor before reacting — that drift-then-kick is the snake.
#define PID_DEADBAND        0.30f
#define DERIV_ALPHA         0.35f  // derivative low-pass: new = a*raw + (1-a)*old
#define CURVE_SLOWDOWN      4.0f   // % base speed removed per unit of |E|
#define MAX_DUTY_STEP       4      // max PWM change per 10 ms tick (slew limit)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

volatile uint8_t uartRxByte    = 0;
volatile uint8_t startCommand  = 0;
volatile uint8_t stopCommand   = 0;
volatile uint8_t reverseCommand = 0;
volatile int     baseSpeed     = DEFAULT_BASE_SPEED;
volatile uint8_t speedChanged  = 0;

// ── Live-tunable PID gains ───────────────────────────────────────────────────
volatile float Kp = KP_DEFAULT;
volatile float Ki = KI_DEFAULT;
volatile float Kd = KD_DEFAULT;
volatile uint8_t pidChanged = 0;  // set in ISR; message is formatted in main loop

RobotState robotState = STOPPED;

uint32_t lastControlTick = 0;
uint32_t lastTelemetryTick = 0;

#define CONTROL_INTERVAL_MS 10U
#define TELEMETRY_INTERVAL_MS 100U

int correction = 0;

int leftDuty = 0;
int rightDuty = 0;
uint8_t sensor[8];
uint8_t lineValid = 0;
volatile uint16_t invalidCount = 0;
float linePosition    = 0.0f;
float lineError       = 0.0f;
float prevError       = 0.0f;
volatile int8_t turnDir = 0; // +1 = RIGHT branch, -1 = LEFT branch
volatile float integralError = 0.0f;   // I-accumulator (reset from ISR on Ki change)
float filteredDeriv   = 0.0f;   // low-passed derivative state
int slewLeft  = 0;              // slew-limited duty actually applied in FOLLOW
int slewRight = 0;
uint32_t reverseHoldUntil = 0;  // non-blocking coast window before reversing
volatile uint8_t swapMotors   = 0;  // 1 = swap Left and Right channels (runtime toggle 'm')
volatile uint8_t flipSensors  = 0;  // 1 = flip S1..S8 indexing (runtime toggle 'k')
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
void PID_Controller(void);
void Read_Sensors(void);
void Calculate_Line_Position(void);
void Set_Motor_PWM(uint8_t left, uint8_t right);
void Set_Motor_Directions(int8_t leftDir, int8_t rightDir);
void Set_Motor_Direction_Forward(void);
void Set_Motor_Direction_Reverse(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    if (uartRxByte == 'S' || uartRxByte == 's') {
      startCommand = 1;
    } else if (uartRxByte == 'X' || uartRxByte == 'x') {
      stopCommand = 1;
    } else if (uartRxByte == 'R' || uartRxByte == 'r' || uartRxByte == 'B' || uartRxByte == 'b') {
      reverseCommand = 1;
    } else if (uartRxByte == '+' || uartRxByte == '=' || uartRxByte == 'u' || uartRxByte == 'U') {
      if (baseSpeed + 5 <= MAX_BASE_SPEED) {
        baseSpeed += 5;
      } else {
        baseSpeed = MAX_BASE_SPEED;
      }
      speedChanged = 1;
    } else if (uartRxByte == '-' || uartRxByte == '_' || uartRxByte == 'd' || uartRxByte == 'D') {
      if (baseSpeed - 5 >= MIN_BASE_SPEED) {
        baseSpeed -= 5;
      } else {
        baseSpeed = MIN_BASE_SPEED;
      }
      speedChanged = 1;
    } else if (uartRxByte >= '1' && uartRxByte <= '9') {
      // Direct speed presets: 1=30%, 2=35%, 3=40%, 4=45%, 5=50%, 6=55%, 7=60%, 8=70%, 9=80%
      static const int presets[] = {30, 35, 40, 45, 50, 55, 60, 70, 80};
      baseSpeed = presets[uartRxByte - '1'];
      speedChanged = 1;

    // ── Live PID gain tuning ────────────────────────────────────────────────
    // No printf in the ISR: just set pidChanged; telemetry_task() reports
    // all three gains, so the web cockpit's PID panel updates on every change.
    // P gain: q=Kp+0.5  Q=Kp+1.0  a=Kp-0.5  A=Kp-1.0
    } else if (uartRxByte == 'q') {
      Kp += 0.5f; pidChanged = 1;
    } else if (uartRxByte == 'Q') {
      Kp += 1.0f; pidChanged = 1;
    } else if (uartRxByte == 'a') {
      if (Kp > 0.5f) { Kp -= 0.5f; }
      pidChanged = 1;
    } else if (uartRxByte == 'A') {
      if (Kp > 1.0f) { Kp -= 1.0f; }
      pidChanged = 1;
    // I gain: e=Ki+0.05  E=Ki+0.2  c=Ki-0.05  C=Ki reset
    } else if (uartRxByte == 'e') {
      Ki += 0.05f; integralError = 0.0f; pidChanged = 1;
    } else if (uartRxByte == 'E') {
      Ki += 0.2f; integralError = 0.0f; pidChanged = 1;
    } else if (uartRxByte == 'c') {
      if (Ki > 0.05f) { Ki -= 0.05f; } else { Ki = 0.0f; }
      integralError = 0.0f; pidChanged = 1;
    } else if (uartRxByte == 'C') {
      Ki = 0.0f; integralError = 0.0f; pidChanged = 1;
    // D gain: w=Kd+1.0  W=Kd+2.0  z=Kd-1.0  Z=Kd-2.0
    } else if (uartRxByte == 'w') {
      Kd += 1.0f; pidChanged = 1;
    } else if (uartRxByte == 'W') {
      Kd += 2.0f; pidChanged = 1;
    } else if (uartRxByte == 'z') {
      if (Kd > 1.0f) { Kd -= 1.0f; }
      pidChanged = 1;
    } else if (uartRxByte == 'Z') {
      if (Kd > 2.0f) { Kd -= 2.0f; } else { Kd = 0.0f; }
      pidChanged = 1;
    // Print all current gains: 'g'
    } else if (uartRxByte == 'g' || uartRxByte == 'G') {
      pidChanged = 1;
    // Toggle Motor Swap: 'm' / 'M'
    } else if (uartRxByte == 'm' || uartRxByte == 'M') {
      swapMotors ^= 1;
      debug_uart_printf(">> MOTOR SWAP=%s\r\n", swapMotors ? "ON" : "OFF");
    // Toggle Sensor Bar Flip: 'k' / 'K'
    } else if (uartRxByte == 'k' || uartRxByte == 'K') {
      flipSensors ^= 1;
      debug_uart_printf(">> SENSOR FLIP=%s\r\n", flipSensors ? "ON" : "OFF");
    }

    HAL_UART_Receive_IT(&huart1, (uint8_t *)&uartRxByte, 1);
  }
}

// Re-arm reception after a UART error (overrun / framing / noise on the BLE
// link). Without this, HAL aborts the IT receive and the robot silently
// ignores every command until the next power cycle.
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                 UART_CLEAR_NEF | UART_CLEAR_PEF);
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&uartRxByte, 1);
  }
}

void line_follow_task(uint32_t now) {
  if ((now - lastControlTick) >= CONTROL_INTERVAL_MS) {
    lastControlTick = now;

    Read_Sensors();
    Calculate_Line_Position();

    // STOP command has priority — always wins
    if (stopCommand) {
      stopCommand = 0;
      startCommand = 0;
      reverseCommand = 0;
      Set_Motor_PWM(0, 0);
      Set_Motor_Directions(1, 1);  // restore forward direction when stopping
      leftDuty = 0;
      rightDuty = 0;
      robotState = STOPPED;
    }

    // Reverse command — works from any state, no sensor needed
    if (reverseCommand) {
      reverseCommand = 0;
      startCommand = 0;
      if (robotState != REVERSE) {
        // Coast 50 ms before reversing — non-blocking (was HAL_Delay(50),
        // which froze the control loop and telemetry).
        Set_Motor_PWM(0, 0);
        Set_Motor_Direction_Reverse();
        reverseHoldUntil = now + 50U;
        robotState = REVERSE;
      } else {
        // Press R again while already reversing → stop + restore forward
        Set_Motor_PWM(0, 0);
        Set_Motor_Direction_Forward();
        leftDuty = 0;
        rightDuty = 0;
        robotState = STOPPED;
      }
    }

    // Line loss recovery: allow up to 3000 ms (300 ticks) of active power-pivot
    // to sweep and re-acquire 90-degree corners or sharp bends before stopping
    if (robotState == FOLLOW) {
      if (!lineValid) {
        if (++invalidCount >= 300) {  // 3.0 s generous timeout
          Set_Motor_PWM(0, 0);
          Set_Motor_Directions(1, 1);
          leftDuty = 0;
          rightDuty = 0;
          robotState = STOPPED;
          startCommand = 0;
          invalidCount = 0;
          turnDir = 0;
        }
      } else {
        invalidCount = 0;
      }
    }

    // Start only when explicitly requested and line is valid
    if (startCommand && lineValid) {
      startCommand = 0;
      invalidCount = 0;
      turnDir       = 0;
      prevError     = 0.0f;   // Reset to 0 so no directional bias on launch
      integralError = 0.0f;   // reset integral on fresh start — prevents windup spike
      filteredDeriv = 0.0f;   // don't carry a stale derivative from the last run
      slewLeft      = baseSpeed;
      slewRight     = baseSpeed;
      Set_Motor_Directions(1, 1);
      robotState = FOLLOW;
    }

    // Apply motor control according to current state
    if (robotState == FOLLOW) {
      PID_Controller();
    } else if (robotState == REVERSE) {
      if ((int32_t)(now - reverseHoldUntil) < 0) {
        Set_Motor_PWM(0, 0);
      } else {
        Set_Motor_PWM((uint8_t)baseSpeed, (uint8_t)baseSpeed);
      }
    } else {
      Set_Motor_PWM(0, 0);
      leftDuty = 0;
      rightDuty = 0;
    }
  }
}
void telemetry_task(uint32_t now) {
  if ((now - lastTelemetryTick) >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTick = now;

    // Priority 1: PID gain change notification
    if (pidChanged) {
      pidChanged = 0;
      debug_uart_printf(">> PID Kp=%.1f Ki=%.2f Kd=%.1f\r\n", Kp, Ki, Kd);
      return;
    }

    // Priority 2: speed change notification
    if (speedChanged) {
      speedChanged = 0;
      debug_uart_printf(">> SPEED=%d%%\r\n", baseSpeed);
      return;
    }

    if (lineValid) {
      debug_uart_printf("VALID,STATE=%s,P=%.2f,E=%.2f,C=%d,L=%d,R=%d\r\n",
                        (robotState == FOLLOW)  ? "FOLLOW"  :
                        (robotState == REVERSE) ? "REVERSE" : "STOPPED",
                        linePosition, lineError, correction, leftDuty,
                        rightDuty);
    } else {
      debug_uart_printf("INVALID,STATE=%s\r\n",
                        (robotState == FOLLOW)  ? "FOLLOW"  :
                        (robotState == REVERSE) ? "REVERSE" : "STOPPED");
    }
  }
}
/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick.
   */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_USART1_UART_Init();
  HAL_UART_Receive_IT(&huart1, (uint8_t *)&uartRxByte, 1);
  /* USER CODE BEGIN 2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);

  // Set both motors to forward direction
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);   // Left IN1
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); // Left IN2

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);   // Right IN3
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET); // Right IN4

  /* USER CODE END 2 */

  /* Initialize USER push-button, will be used to trigger an interrupt each time
   * it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {

    uint32_t now = HAL_GetTick();

    line_follow_task(now);
    telemetry_task(now);

    debug_uart_task();
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
   */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) {
    Error_Handler();
  }
}

/**
 * @brief TIM2 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM2_Init(void) {

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 169;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 99;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK) {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);
}

/**
 * @brief TIM3 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM3_Init(void) {

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 169;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 99;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK) {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK) {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK) {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);
}

/**
 * @brief USART1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_USART1_UART_Init(void) {

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */
}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_5 | GPIO_PIN_6,
                    GPIO_PIN_RESET);

  /*Configure GPIO pins : PA0 PA1 PA5 PA6 */
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_5 | GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PC7 */
  GPIO_InitStruct.Pin = GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA8 PA9 PA10 */
  GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB3 PB4 PB5 PB6 */
  GPIO_InitStruct.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

static int Slew(int current, int target) {
  if (target > current + MAX_DUTY_STEP) return current + MAX_DUTY_STEP;
  if (target < current - MAX_DUTY_STEP) return current - MAX_DUTY_STEP;
  return target;
}

void PID_Controller(void) {
  if (!lineValid) {
    // ── Active Line Loss / Sharp Bend Recovery ───────────────────────────
    integralError = 0.0f;
    filteredDeriv = 0.0f;
    // Re-acquire from cruise speed, not from the pivot duties
    slewLeft  = baseSpeed;
    slewRight = baseSpeed;

    // First 150ms (15 ticks): drive straight forward to clear any brief sensor drop,
    // surface dip, or tape gap. Also keep driving straight if we were not actively turning!
    if (invalidCount <= 15 || turnDir == 0) {
      Set_Motor_Directions(1, 1);
      Set_Motor_PWM((uint8_t)baseSpeed, (uint8_t)baseSpeed);
      leftDuty = baseSpeed;
      rightDuty = baseSpeed;
      return;
    }

    // After 150ms of confirmed line loss ONLY if we were actively turning:
    // execute active corner search pivot in the direction of the turn
    if (turnDir == +1) {
      // ── 90° RIGHT TURN PIVOT ──────────────────────────────────────────
      Set_Motor_Directions(+1, -1);
      Set_Motor_PWM(80, 45);
      leftDuty = 80;
      rightDuty = -45;
    } else if (turnDir == -1) {
      // ── 90° LEFT TURN PIVOT ───────────────────────────────────────────
      Set_Motor_Directions(-1, +1);
      Set_Motor_PWM(45, 80);
      leftDuty = -45;
      rightDuty = 80;
    }
    return;
  }

  // ── Line is valid: Ensure Forward motor directions ─────────────────────
  Set_Motor_Directions(1, 1);

  float absError = (lineError >= 0.0f) ? lineError : -lineError;

  // ── Derivative: updated EVERY tick so the filter never holds a stale value
  // (previously it froze inside the deadband and fired on the next exit).
  float rawDeriv = lineError - prevError;
  prevError = lineError;
  filteredDeriv = DERIV_ALPHA * rawDeriv + (1.0f - DERIV_ALPHA) * filteredDeriv;

  // ── Integral with anti-windup clamp (Ki = 0 by default → no effect) ─────
  if (Ki > 0.0f) {
    integralError += lineError;
    if (integralError > INTEGRAL_LIMIT)  integralError = INTEGRAL_LIMIT;
    if (integralError < -INTEGRAL_LIMIT) integralError = -INTEGRAL_LIMIT;
  } else {
    integralError = 0.0f;
  }
  float i_term = Ki * integralError;

  float corr;
  if (absError < PID_DEADBAND) {
    corr = i_term;  // centred: no P/D kick, only slow trim
  } else {
    corr = Kp * lineError + Kd * filteredDeriv + i_term;
  }

  if (corr > MAX_CORRECTION)  corr = MAX_CORRECTION;
  if (corr < -MAX_CORRECTION) corr = -MAX_CORRECTION;

  // Round instead of truncate (truncation threw away up to 1% each tick)
  correction = (int)((corr >= 0.0f) ? (corr + 0.5f) : (corr - 0.5f));

  // ── Stall floor scales with base speed ─────────────────────────────────
  // At the default 70% this is exactly MIN_PWM (55). At low presets it drops
  // so 30–50% speeds actually run slower instead of being clamped up to 55%.
  int pwmFloor = (baseSpeed * MIN_PWM) / DEFAULT_BASE_SPEED;
  if (pwmFloor > MIN_PWM) pwmFloor = MIN_PWM;

  // ── Slow down in curves: less speed → less overshoot → less snaking ────
  int cruise = baseSpeed - (int)(CURVE_SLOWDOWN * absError);
  if (cruise < pwmFloor) cruise = pwmFloor;

  int lTarget = cruise + correction;
  int rTarget = cruise - correction;

  if (lTarget > MAX_PWM)  lTarget = MAX_PWM;
  if (rTarget > MAX_PWM)  rTarget = MAX_PWM;
  if (lTarget < pwmFloor) lTarget = pwmFloor;
  if (rTarget < pwmFloor) rTarget = pwmFloor;

  // ── Slew-rate limit: no instant PWM jumps → no steering "kicks" ────────
  slewLeft  = Slew(slewLeft,  lTarget);
  slewRight = Slew(slewRight, rTarget);

  leftDuty  = slewLeft;
  rightDuty = slewRight;

  Set_Motor_PWM((uint8_t)leftDuty, (uint8_t)rightDuty);
}

void Set_Motor_PWM(uint8_t left, uint8_t right) {
  if (swapMotors) {
    uint8_t tmp = left;
    left = right;
    right = tmp;
  }

  if (left > 100)
    left = 100;
  if (right > 100)
    right = 100;

  uint32_t leftARR = __HAL_TIM_GET_AUTORELOAD(&htim3);
  uint32_t rightARR = __HAL_TIM_GET_AUTORELOAD(&htim2);

  uint32_t leftCompare = (left * (leftARR + 1)) / 100;
  uint32_t rightCompare = (right * (rightARR + 1)) / 100;

  if (leftCompare > leftARR)
    leftCompare = leftARR;
  if (rightCompare > rightARR)
    rightCompare = rightARR;

  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, leftCompare);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, rightCompare);
}

void Set_Motor_Directions(int8_t leftDir, int8_t rightDir) {
  if (swapMotors) {
    int8_t tmp = leftDir;
    leftDir = rightDir;
    rightDir = tmp;
  }

  // Left motor: IN1 (PA6), IN2 (PA5)
  if (leftDir > 0) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
  } else if (leftDir < 0) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
  } else {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
  }

  // Right motor: IN3 (PA0), IN4 (PA1)
  if (rightDir > 0) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
  } else if (rightDir < 0) {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  } else {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
  }
}

void Set_Motor_Direction_Forward(void) {
  Set_Motor_Directions(1, 1);
}

void Set_Motor_Direction_Reverse(void) {
  Set_Motor_Directions(-1, -1);
}

void Calculate_Line_Position(void) {
  int sum = 0;
  int count = 0;

  for (int i = 0; i < 8; i++) {
    if (sensor[i] == 1) {
      sum += i;
      count++;
    }
  }

  // ── Corner Recovery Fast Re-Acquisition ────────────────────────────────
  // When in active corner pivot (invalidCount > 0), as soon as any center
  // sensor (2, 3, 4, 5) sweeps across black tape, lock onto the new line immediately!
  if (robotState == FOLLOW && invalidCount > 0 && count > 0) {
    if (sensor[2] == 1 || sensor[3] == 1 || sensor[4] == 1 || sensor[5] == 1) {
      lineValid = 1;
      invalidCount = 0;
      turnDir = 0;
      linePosition = (float)sum / count;
      lineError = linePosition - 3.5f;
      prevError = lineError;
      return;
    }
  }

  // No sensor detects black (lost line / overshoot)
  if (count == 0) {
    lineValid = 0;
    return;
  }

  // ── Grid Intersection / Cross-Line Detection ───────────────────────────
  // When crossing perpendicular grid lines, 5 or more sensors see black tape.
  // If center sensors see black, the forward track continues straight ahead!
  if (count >= 5) {
    if (sensor[3] == 1 || sensor[4] == 1 || (sensor[2] == 1 && sensor[5] == 1)) {
      lineValid = 1;
      linePosition = 3.5f;  // Lock dead-center forward across the cross-line
      lineError = 0.0f;
      // Maintain straight forward heading through grid intersection
      return;
    }
  }

  // ── Normal Line Tracking ───────────────────────────────────────────────
  lineValid = 1;
  linePosition = (float)sum / count;
  lineError = linePosition - 3.5f;

  // Latch turn direction on significant lateral deviation
  if (lineError > 1.2f) {
    turnDir = +1;
  } else if (lineError < -1.2f) {
    turnDir = -1;
  } else if (lineError > -0.6f && lineError < 0.6f) {
    turnDir = 0;  // Centered on the line again — clear turn latch!
  }
}

void Read_Sensors(void) {
  uint8_t raw[8];
  raw[0] = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10);
  raw[1] = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3);
  raw[2] = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5);
  raw[3] = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4);

  raw[4] = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8);
  raw[5] = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9);
  raw[6] = HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_7);
  raw[7] = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_6);

  if (flipSensors) {
    for (int i = 0; i < 8; i++) {
      sensor[i] = raw[7 - i];
    }
  } else {
    for (int i = 0; i < 8; i++) {
      sensor[i] = raw[i];
    }
  }
}
/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
