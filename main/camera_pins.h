#ifndef CAMERA_PINS_H
#define CAMERA_PINS_H

// Freenove ESP32-S3 WROOM Camera Module Pins
// Using CAMERA_MODEL_ESP32S3_EYE pin configuration

#define CAM_PIN_PWDN    -1  // Power down (not used)
#define CAM_PIN_RESET   -1  // Reset (not used) 
#define CAM_PIN_XCLK    15  // Master clock
#define CAM_PIN_SIOD     4  // I2C SDA (matches ESP32S3_EYE)
#define CAM_PIN_SIOC     5  // I2C SCL (matches ESP32S3_EYE)

// Data pins - Y2 through Y9 naming to match ESP32S3_EYE convention
#define CAM_PIN_D2      11  // Y2 - D0 (LSB)
#define CAM_PIN_D3       9  // Y3 - D1
#define CAM_PIN_D4       8  // Y4 - D2
#define CAM_PIN_D5      10  // Y5 - D3
#define CAM_PIN_D6      12  // Y6 - D4
#define CAM_PIN_D7      18  // Y7 - D5
#define CAM_PIN_D8      17  // Y8 - D6
#define CAM_PIN_D9      16  // Y9 - D7 (MSB)

#define CAM_PIN_VSYNC    6  // Vertical sync
#define CAM_PIN_HREF     7  // Horizontal reference
#define CAM_PIN_PCLK    13  // Pixel clock

#endif // CAMERA_PINS_H

