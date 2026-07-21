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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "arm_math.h"
#include <string.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define SAMPLES 1024
#define HARMONICS 25

// DSP Calibration (Multiplying by inverse is faster than division)
#define V_CALIBRATION_FACTOR (1.0f / 63.9204f)
#define I_CALIBRATION_FACTOR (1.0f / 432.5244f)
#define SAMPLING_FREQ 10000.0f

//static uint32_t tx_start_tick = 0;

typedef struct __attribute__((packed)) {
    uint32_t  start_byte;
    uint32_t  seq;

    int16_t  voltage[SAMPLES];
    int16_t  current[SAMPLES];

    float    v_rms, i_rms;
    float    frequency;
    float    power_factor;
    float    active_power, apparent_power, reactive_power;
    float    crest_factor_v, crest_factor_i;
    float    swell_factor;
    float    thd_v, thd_i;
    float    harmonics_v[HARMONICS];
    float    harmonics_i[HARMONICS];

    uint16_t checksum;
} WaveformPacket_t;   // 4351 bytes total

static WaveformPacket_t pkt;
static volatile uint8_t tx_busy = 0;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_rx;
DMA_HandleTypeDef hdma_spi1_tx;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart6;
DMA_HandleTypeDef hdma_usart6_tx;

/* USER CODE BEGIN PV */
int16_t dma_rx_buffer[5]; // to store each 100us SPI output of first 5 channel

//For testing Only
int16_t dummy_tx[5] = {0, 0, 0, 0, 0};
volatile uint32_t dma_errors = 0;
volatile HAL_StatusTypeDef last_spi_status = HAL_OK;


int16_t voltage_A[1024];
int16_t current_A[1024];
int16_t voltage_B[1024];
int16_t current_B[1024];

//Parameters for tracking
volatile uint16_t local_sample = 0;
volatile uint8_t fill_half_A = 1;   // 1 = Writing to A arrays, 0 = Writing to B arrays

volatile uint8_t process_half_A = 0;
volatile uint8_t process_half_B = 0;

volatile uint8_t process_buffer_flag = 0;
uint32_t seq_counter = 0;

// DSP Static Arrays (Kept in global RAM to save stack space)
static float32_t v_float[SAMPLES];
static float32_t i_float[SAMPLES];

static float32_t fft_output_v[SAMPLES];
static float32_t magnitudes_v[SAMPLES / 2];

static float32_t fft_output_i[SAMPLES];
static float32_t magnitudes_i[SAMPLES / 2];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART6_UART_Init(void);
/* USER CODE BEGIN PFP */


static uint16_t compute_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

// The Real DSP Function
void Calculate_Power_Quality(WaveformPacket_t* p)
{
    float32_t active_power_sum = 0.0f;
    float32_t v_max = 0.0f;
    float32_t i_max = 0.0f;

    float32_t temp_v_rms = 0.0f;
    float32_t temp_i_rms = 0.0f;
    float32_t temp_reactive = 0.0f;
    float32_t temp_thd_v = 0.0f;
    float32_t temp_thd_i = 0.0f;

    // 1. CONVERT, SCALE, AND FIND PEAKS
    for (uint16_t i = 0; i < SAMPLES; i++)
    {
        v_float[i] = (float32_t)p->voltage[i] * V_CALIBRATION_FACTOR;
        i_float[i] = (float32_t)p->current[i] * I_CALIBRATION_FACTOR;

        if (fabsf(v_float[i]) > v_max) v_max = fabsf(v_float[i]);
        if (fabsf(i_float[i]) > i_max) i_max = fabsf(i_float[i]);
    }

    // 2. RMS & CREST FACTOR
    arm_rms_f32(v_float, SAMPLES, &temp_v_rms);
    arm_rms_f32(i_float, SAMPLES, &temp_i_rms);

    p->v_rms = temp_v_rms;
    p->i_rms = temp_i_rms;

    p->crest_factor_v = (p->v_rms > 0.1f) ? (v_max / p->v_rms) : 0.0f;
    p->crest_factor_i = (p->i_rms > 0.01f) ? (i_max / p->i_rms) : 0.0f;

    // 3. POWER CALCULATIONS
    arm_dot_prod_f32(v_float, i_float, SAMPLES, &active_power_sum);
    p->active_power = active_power_sum / (float32_t)SAMPLES;
    p->apparent_power = p->v_rms * p->i_rms;

    p->power_factor = (p->apparent_power > 0.1f) ? (p->active_power / p->apparent_power) : 0.0f;
    if (p->power_factor > 1.0f) p->power_factor = 1.0f;
    if (p->power_factor < -1.0f) p->power_factor = -1.0f;

    float32_t q_sq = (p->apparent_power * p->apparent_power) - (p->active_power * p->active_power);
    arm_sqrt_f32((q_sq > 0.0f) ? q_sq : 0.0f, &temp_reactive);
    p->reactive_power = temp_reactive;

    // 4. FFT ON VOLTAGE AND CURRENT
    // (init once, not every call — arm_rfft_fast_init_f32 just fills a struct with twiddle
    //  factor pointers, it's wasted work to redo it on every single packet)
    static arm_rfft_fast_instance_f32 fft_inst;
    static uint8_t fft_initialized = 0;
    if (!fft_initialized) {
        arm_rfft_fast_init_f32(&fft_inst, SAMPLES);
        fft_initialized = 1;
    }

    arm_rfft_fast_f32(&fft_inst, v_float, fft_output_v, 0);
    arm_cmplx_mag_f32(fft_output_v, magnitudes_v, SAMPLES / 2);

    arm_rfft_fast_f32(&fft_inst, i_float, fft_output_i, 0);
    arm_cmplx_mag_f32(fft_output_i, magnitudes_i, SAMPLES / 2);

    // 5. FIND THE FUNDAMENTAL — full-spectrum search (bin 0 / DC excluded)
    // Voltage is used as the reference for the fundamental bin. Current is often far more
    // distorted (nonlinear loads, switching supplies) and its true peak bin can drift away
    // from the real fundamental, so anchoring both channels' harmonic numbering to the
    // voltage fundamental is the standard approach in power quality analyzers.
    float32_t max_mag = 0.0f;
    uint32_t fundamental_bin = 1;

    for (uint32_t b = 1; b < (SAMPLES / 2); b++) {
        if (magnitudes_v[b] > max_mag) {
            max_mag = magnitudes_v[b];
            fundamental_bin = b;
        }
    }

    // Parabolic interpolation across the 3 bins around the peak for sub-bin frequency
    // resolution. Raw bin spacing is SAMPLING_FREQ/SAMPLES ≈ 9.77 Hz here, which is coarse
    // enough that 49 Hz and 51 Hz could otherwise land in the same bin.
    float32_t bin_freq = SAMPLING_FREQ / (float32_t)SAMPLES;
    float32_t true_bin = (float32_t)fundamental_bin;
    if (fundamental_bin > 1 && fundamental_bin < (SAMPLES / 2 - 1)) {
        float32_t alpha = magnitudes_v[fundamental_bin - 1];
        float32_t beta  = magnitudes_v[fundamental_bin];
        float32_t gamma = magnitudes_v[fundamental_bin + 1];
        float32_t denom = (alpha - 2.0f * beta + gamma);
        if (fabsf(denom) > 1e-9f) {
            float32_t offset = 0.5f * (alpha - gamma) / denom;
            if (offset > 0.5f) offset = 0.5f;
            if (offset < -0.5f) offset = -0.5f;
            true_bin = (float32_t)fundamental_bin + offset;
        }
    }

    p->frequency = true_bin * bin_freq;

    // 6. HARMONICS + THD — VOLTAGE
    float32_t harmonic_sq_sum_v = 0.0f;
    for (int h = 0; h < HARMONICS; h++)
    {
        uint32_t target_bin = fundamental_bin * (h + 1);
        if (target_bin < (SAMPLES / 2)) {
            p->harmonics_v[h] = magnitudes_v[target_bin] * (2.0f / (float32_t)SAMPLES);
        } else {
            p->harmonics_v[h] = 0.0f;
        }
        if (h > 0) harmonic_sq_sum_v += (p->harmonics_v[h] * p->harmonics_v[h]);
    }

    float32_t fundamental_rms_v = p->harmonics_v[0] / 1.41421356f;
    if (fundamental_rms_v > 1.0f) {
        arm_sqrt_f32(harmonic_sq_sum_v, &temp_thd_v);
        p->thd_v = (temp_thd_v / 1.41421356f) / fundamental_rms_v * 100.0f;
    } else {
        p->thd_v = 0.0f;
    }

    // 7. HARMONICS + THD — CURRENT (same fundamental_bin as voltage — see note above)
    float32_t harmonic_sq_sum_i = 0.0f;
    for (int h = 0; h < HARMONICS; h++)
    {
        uint32_t target_bin = fundamental_bin * (h + 1);
        if (target_bin < (SAMPLES / 2)) {
            p->harmonics_i[h] = magnitudes_i[target_bin] * (2.0f / (float32_t)SAMPLES);
        } else {
            p->harmonics_i[h] = 0.0f;
        }
        if (h > 0) harmonic_sq_sum_i += (p->harmonics_i[h] * p->harmonics_i[h]);
    }

    // Current amplitude scale is very different from voltage (amps vs volts), so the
    // "is there even a signal here" threshold needs to be much smaller than 1.0f.
    // Tune this to whatever your minimum expected load current actually is.
    float32_t fundamental_rms_i = p->harmonics_i[0] / 1.41421356f;
    if (fundamental_rms_i > 0.01f) {
        arm_sqrt_f32(harmonic_sq_sum_i, &temp_thd_i);
        p->thd_i = (temp_thd_i / 1.41421356f) / fundamental_rms_i * 100.0f;
    } else {
        p->thd_i = 0.0f;
    }
}

// --- YOUR PROVEN CALLBACKS ---

//void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
//{
//    if (htim->Instance == TIM2) {
//        // ONLY SET THE FLAG HERE. DO NOT DO MATH!
//        if (!tx_busy) {
//            process_buffer_flag = 1;
//        }
//    }
//}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART6) {
        //tx_duration_ms = HAL_GetTick() - tx_start_tick;
        tx_busy = 0;
    }
}

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_TIM2_Init();
  MX_USART6_UART_Init();
  /* USER CODE BEGIN 2 */

  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET); //To prevent ADC to think it's ready to talk(Need confirmation)

  HAL_NVIC_DisableIRQ(EXTI1_IRQn); //Disable Busy interrupts before Reseting the ADC as ADC resets ADC may output unpredictable Busy signals making DMA fires at unwanted stage
  //Start the ADC
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_SET);
  HAL_Delay(1);   // Hold reset for 1 millisecond
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);
  HAL_Delay(10);  // Give the ADC 10ms to wake up and stabilize

  __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_1);

    // 5. UN-MUTE THE ALARM: Re-enable EXTI Line 1
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  //Start PWM
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

	if (process_half_A == 1)
	{
		process_half_A = 0; // Clear the flag immediately so we don't double-process

		// 1. Wait for the previous USART transmission to finish (if it hasn't already)
		while (tx_busy == 1)
		{
			// CPU spins here for a fraction of a millisecond
		}

		// 2. Setup Packet Headers
		pkt.start_byte = 0xAA55AA55;
		pkt.seq = seq_counter++;

		// 3. Load the fresh raw ADC data into the packet struct
		memcpy(pkt.voltage, voltage_A, sizeof(voltage_A));
		memcpy(pkt.current, current_A, sizeof(current_A));

		// 4. Run the Hardware FPU Math
		// (This crunches the data sitting inside pkt.voltage and pkt.current)
		Calculate_Power_Quality(&pkt);

		// 5. Secure it with the checksum
		pkt.checksum = compute_checksum((uint8_t*)&pkt, sizeof(pkt) - sizeof(pkt.checksum));

		// 6. Blast the completed packet out over USART DMA
		tx_busy = 1;
		HAL_UART_Transmit_DMA(&huart6, (uint8_t*)&pkt, sizeof(pkt));
	}

	// ---------------------------------------------------------
	// BUFFER B IS FULL
	// ---------------------------------------------------------
	else if (process_half_B == 1)
	{
		process_half_B = 0;

		while (tx_busy == 1) {}

		pkt.start_byte = 0xAA55AA55;
		pkt.seq = seq_counter++;

		// Load the B buffer data this time!
		memcpy(pkt.voltage, voltage_B, sizeof(voltage_B));
		memcpy(pkt.current, current_B, sizeof(current_B));

		Calculate_Power_Quality(&pkt);

		pkt.checksum = compute_checksum((uint8_t*)&pkt, sizeof(pkt) - sizeof(pkt.checksum));

		tx_busy = 1;
		HAL_UART_Transmit_DMA(&huart6, (uint8_t*)&pkt, sizeof(pkt));
	}
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 12;
  RCC_OscInitStruct.PLL.PLLN = 96;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 99;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 99;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 10;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief USART6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART6_UART_Init(void)
{

  /* USER CODE BEGIN USART6_Init 0 */

  /* USER CODE END USART6_Init 0 */

  /* USER CODE BEGIN USART6_Init 1 */

  /* USER CODE END USART6_Init 1 */
  huart6.Instance = USART6;
  huart6.Init.BaudRate = 921600;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART6_Init 2 */

  /* USER CODE END USART6_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  /* DMA2_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, GPIO_PIN_RESET);

  /*Configure GPIO pin : PA1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PA2 PA3 */
  GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI1_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

//--------Catch interrupt from falling edge of Busy Pin----------//
//void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
//{
//    // Check if the interrupt came from the BUSY pin (PA1)
//    if (GPIO_Pin == GPIO_PIN_1)
//    {
//        // 1. WAKE UP ADC: Pull CS (PA2) LOW
//        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
//
//        // 2. FETCH DATA: Tell DMA to grab exactly 5 words (10 bytes)
//        //HAL_SPI_Receive_DMA(&hspi1, (uint8_t*)dma_rx_buffer, 5);
//        HAL_SPI_TransmitReceive_DMA(&hspi1, (uint8_t*)dummy_tx, (uint8_t*)dma_rx_buffer, 5);
//    }
//}
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_1)
    {
        // SAFEGUARD: Only initiate a new DMA transfer if the SPI is ready
        if (hspi1.State == HAL_SPI_STATE_READY)
        {
            // 1. WAKE UP ADC: Pull CS (PA2) LOW
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);

            // Capture the return status explicitly
            last_spi_status = HAL_SPI_TransmitReceive_DMA(&hspi1, (uint8_t*)dummy_tx, (uint8_t*)dma_rx_buffer, 5);

            if (last_spi_status != HAL_OK)
            {
                dma_errors++; // Keep your breakpoint here to catch errors
            }
        }
    }
}

//----------Catch the interrupt from DMA and arrange them-----------//
//void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
    {
        // 1. PUT ADC TO SLEEP
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);

        // 2. ROUTE THE DATA TO THE CORRECT ARRAYS
        if (fill_half_A == 1)
        {
            current_A[local_sample] = dma_rx_buffer[4];
            voltage_A[local_sample] = dma_rx_buffer[0];
        }
        else
        {
            current_B[local_sample] = dma_rx_buffer[4];
            voltage_B[local_sample] = dma_rx_buffer[0];
        }

        local_sample++;

        // 3. THE STOPWATCH LOGIC
        if (local_sample >= 1024)
        {
            local_sample = 0; // Reset for the next batch

            if (fill_half_A == 1)
            {
                process_half_A = 1;  // Tell main() that A is full
                fill_half_A = 0;     // Switch the hardware to fill B next
            }
            else
            {
                process_half_B = 1;  // Tell main() that B is full
                fill_half_A = 1;     // Switch the hardware to fill A next
            }
        }
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
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
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
