# Smart Power Quality Analyzer

## Overview

The Smart Power Quality Analyzer is an embedded measurement and monitoring system developed for real-time analysis of electrical power quality in single-phase AC power systems. The device continuously acquires voltage and current waveforms, performs high-speed digital signal processing, and provides comprehensive power quality parameters through an intuitive graphical user interface and wireless monitoring platform.

The system is designed around a dual-microcontroller architecture consisting of an STM32H7 as the primary real-time processing unit and an ESP32-S3 dedicated to user interface management, wireless communication, and data visualization. By separating deterministic signal processing from networking and display tasks, the analyzer achieves high measurement accuracy while maintaining a responsive user interface.

The project combines precision analog signal conditioning, high-speed data acquisition, embedded DSP algorithms, graphical visualization, PCB design, and IoT connectivity into a single integrated platform suitable for laboratory, industrial, and educational applications.


# Key Features

## Electrical Measurements

- True RMS voltage measurement
- True RMS current measurement
- Active power calculation
- Reactive power calculation
- Apparent power calculation
- Power factor measurement
- Grid frequency measurement
- Phase angle calculation


## Power Quality Analysis

- Fast Fourier Transform (FFT) based spectral analysis
- Total Harmonic Distortion (THD) calculation
- Individual harmonic magnitude analysis
- Harmonic spectrum visualization
- Real-time waveform monitoring
- Voltage and current waveform comparison
- Continuous sampling and processing


## User Interface

- 4-inch SPI TFT LCD graphical display
- Rotary encoder based navigation
- Physical control buttons for menu navigation
- Real-time waveform rendering
- Live numerical parameter display
- Interactive graphical user interface
- Adjustable display brightness

## Connectivity

- Wi-Fi connectivity
- UART communication between STM32 and ESP32
- Remote monitoring
- Web-based dashboard
- Wireless data transfer
- MicroSD card support for future data logging


## Embedded System Features

- High-speed ADC sampling
- Real-time DSP processing
- Interrupt-driven firmware
- Multi-tasking architecture
- High-speed UART communication
- Hardware PWM control
- DMA-based data acquisition
- Efficient memory management


# System Architecture

The analyzer is divided into two independent processing sections.

## STM32H7 Processing Unit

Responsible for:

- High-speed ADC sampling
- Analog signal acquisition
- Digital filtering
- FFT computation
- RMS calculations
- Power calculations
- Harmonic analysis
- Frequency estimation
- Real-time data processing
- Communication with ESP32


## ESP32-S3 Interface Unit

Responsible for:

- LCD graphics rendering
- User interface management
- Rotary encoder handling
- Push-button interface
- Wi-Fi communication
- Web server communication
- Remote monitoring
- Data visualization
- System configuration


# Hardware Overview

## Analog Front-End

- Voltage sensing circuitry
- Current sensing circuitry
- Signal conditioning
- Precision operational amplifiers
- Anti-aliasing filters
- ADC input protection


## Processing Hardware

### STM32F4

- Primary real-time processing controller
- Digital signal processing
- High-speed ADC operation
- FFT execution
- Measurement calculations

### ESP32-S3

- Dual-core processor
- Integrated Wi-Fi
- TFT display controller
- Human-machine interface
- Wireless communication


## Display

- 4.0-inch SPI TFT LCD
- Real-time waveform visualization
- Menu-based graphical interface
- Adjustable LED backlight


## User Controls

- Incremental rotary encoder
- Enter button
- Back button
- System power control
- On-screen navigation


## Power System

- High-efficiency buck converters
- Low-noise analog supplies
- Dedicated digital power rails
- Ferrite-bead power filtering
- Controlled power sequencing
- Push-button power management


# Signal Processing

The system continuously samples voltage and current waveforms using the STM32F4 ADC.

The acquired samples undergo several DSP stages including:

- Offset correction
- Noise filtering
- Windowing
- FFT transformation
- Harmonic extraction
- RMS computation
- Frequency estimation
- Power calculations
- THD computation

The processed information is transmitted to the ESP32-S3 for visualization and wireless communication.


# Communication Architecture

## STM32 ↔ ESP32

Communication is performed through an RS-485 interface, providing reliable high-speed data transfer between the processing unit and the user interface module.

Transmitted data includes:

- Voltage samples
- Current samples
- RMS measurements
- Frequency
- Power factor
- Harmonic information
- THD values
- System status


## ESP32 ↔ User

The ESP32 provides:

- Graphical LCD interface
- Local parameter display
- Wi-Fi connectivity
- Remote monitoring capability
- Configuration interface


# PCB Design

The hardware is implemented using a custom multilayer PCB designed for mixed-signal applications.

The PCB incorporates:

- Dedicated analog and digital sections
- Controlled grounding strategy
- High-speed signal routing
- Power plane optimization
- EMI reduction techniques
- Differential USB routing
- SPI display interface
- RS-485 communication interface


# Software Stack

## STM32 Firmware

- C
- STM32CubeIDE
- HAL drivers
- CMSIS
- CMSIS-DSP
- Interrupt-driven architecture
- DMA-based ADC acquisition


## ESP32 Firmware

- Arduino Framework
- C++
- Native USB programming
- TFT graphics library
- Wi-Fi stack
- UART/RS-485 communication


# Development Tools

- STM32CubeIDE
- Arduino IDE
- Altium Designer
- SolidWorks


# Future Enhancements

- Three-phase power quality analysis
- Cloud connectivity
- Historical data visualization
- Energy consumption analytics
- Remote firmware updates (OTA)
- Modbus support
- USB data export
- Advanced event logging
- Configurable alarm thresholds
- Industrial communication protocols


# Current Status

**Development Status:** Ongoing (Final Stage)

The hardware architecture, embedded firmware, PCB design, communication framework, and graphical user interface are in the final stages of development and integration.
