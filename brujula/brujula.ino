#include <Arduino.h>
#include "M5Stack.h"
#include "M5_BMM150.h"
#include "M5_BMM150_DEFS.h"
#include "Preferences.h"
#include "math.h"

// Constantes y definiciones
#define CIRCULAR_BUFFER_LEN 100   // Longitud máxima del buffer circular
#define AVERAGE_COUNT_DEFAULT 10  // Valor por defecto para promediar muestras

// Opciones del menú
enum MenuOption {
  MENU_CALIBRATION_TIME,
  MENU_CALIBRATE,
  MENU_EXIT
};

// Estructura para el buffer circular
typedef struct {
  int head;                           // Cabeza del buffer
  int tail;                           // Cola del buffer
  float values[CIRCULAR_BUFFER_LEN];  // Valores almacenados
  int count;                          // Número actual de elementos en el buffer
} circular_buffer;

// Función para limpiar el buffer
void value_clear(circular_buffer *buf) {
  buf->head = 0;
  buf->tail = 0;
  buf->count = 0;
  for (int i = 0; i < CIRCULAR_BUFFER_LEN; i++) {
    buf->values[i] = 0.0;
  }
}

// Función para añadir un valor al buffer
void value_queue(circular_buffer *buf, float value) {
  buf->values[buf->head] = value;
  buf->head = (buf->head + 1) % CIRCULAR_BUFFER_LEN;
  if (buf->count < CIRCULAR_BUFFER_LEN) {
    buf->count++;
  } else {
    buf->tail = (buf->tail + 1) % CIRCULAR_BUFFER_LEN;
  }
}

// Función para calcular el promedio de los valores en el buffer
float value_average(const circular_buffer *buf, int average_count) {
  if (buf->count == 0) return 0.0;

  int actual_count = (average_count > buf->count) ? buf->count : average_count;
  float sum = 0.0;
  int index = buf->head - 1;
  if (index < 0) index += CIRCULAR_BUFFER_LEN;

  for (int i = 0; i < actual_count; i++) {
    sum += buf->values[index];
    index--;
    if (index < 0) index += CIRCULAR_BUFFER_LEN;
  }

  return sum / actual_count;
}

// Variables globales
Preferences prefs;                                  // Gestión de memoria no volátil para guardar calibraciones
struct bmm150_dev dev;                              // Configuración del sensor BMM150
bmm150_mag_data mag_offset;                         // Datos de compensación del magnetómetro
bmm150_mag_data mag_max;                            // Máximos registrados durante la calibración
bmm150_mag_data mag_min;                            // Mínimos registrados durante la calibración
TFT_eSprite compass_sprite = TFT_eSprite(&M5.Lcd);  // Sprite para la brújula

circular_buffer buffer_x, buffer_y, buffer_z;  // Buffers circulares para datos del magnetómetro
int AVERAGE_COUNT = AVERAGE_COUNT_DEFAULT;     // Número de muestras para promediar

bool isMenuActive = false;                             // Indica si el menú está activo
MenuOption currentMenuOption = MENU_CALIBRATION_TIME;  // Opción seleccionada
uint32_t calibrationTime = 5000;                       // Tiempo de calibración por defecto (en ms)

// Funciones auxiliares de I2C
int8_t i2c_read(uint8_t dev_id, uint8_t reg_addr, uint8_t *read_data, uint16_t len) {
  return (M5.I2C.readBytes(dev_id, reg_addr, len, read_data)) ? BMM150_OK : BMM150_E_DEV_NOT_FOUND;
}

int8_t i2c_write(uint8_t dev_id, uint8_t reg_addr, uint8_t *write_data, uint16_t len) {
  return (M5.I2C.writeBytes(dev_id, reg_addr, write_data, len)) ? BMM150_OK : BMM150_E_DEV_NOT_FOUND;
}

// Funciones auxiliares del menú
void drawMenu() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextColor(TFT_PINK);
  M5.Lcd.setTextSize(2);

  // Opción 1: Tiempo de calibración
  M5.Lcd.setCursor(10, 50);
  if (currentMenuOption == MENU_CALIBRATION_TIME) {
    M5.Lcd.print("> Aumentar tiempo de");
    M5.Lcd.setCursor(10, 70);
    M5.Lcd.print("  calibracion 5s");
  } else {
    M5.Lcd.print("Aumentar tiempo de");
    M5.Lcd.setCursor(10, 70);
    M5.Lcd.print("calibracion 5s");
  }

  // Opción 2: Calibrar
  M5.Lcd.setCursor(10, 120);
  M5.Lcd.print(currentMenuOption == MENU_CALIBRATE ? "> Calibrar" : "Calibrar");

  // Opción 3: Salir
  M5.Lcd.setCursor(10, 170);
  M5.Lcd.print(currentMenuOption == MENU_EXIT ? "> Salir" : "Salir");
}

// Guarda los datos de calibración en memoria
void bmm150_offset_save() {
  prefs.begin("bmm150", false);
  prefs.putBytes("offset", (uint8_t *)&mag_offset, sizeof(bmm150_mag_data));
  prefs.end();
}

void bmm150_calibrate(uint32_t calibrate_time) {  // Calibra los datos de bmm150
  uint32_t calibrate_timeout = 0;

  calibrate_timeout = millis() + calibrate_time;
  Serial.printf("Calibración de %d ms \r\n", calibrate_time);
  Serial.printf("ejecutando...");

  while (calibrate_timeout > millis()) {
    bmm150_read_mag_data(&dev);  // Lee los datos del magnetómetro
    if (dev.data.x) {
      mag_min.x = (dev.data.x < mag_min.x) ? dev.data.x : mag_min.x;
      mag_max.x = (dev.data.x > mag_max.x) ? dev.data.x : mag_max.x;
    }
    if (dev.data.y) {
      mag_max.y = (dev.data.y > mag_max.y) ? dev.data.y : mag_max.y;
      mag_min.y = (dev.data.y < mag_min.y) ? dev.data.y : mag_min.y;
    }
    if (dev.data.z) {
      mag_min.z = (dev.data.z < mag_min.z) ? dev.data.z : mag_min.z;
      mag_max.z = (dev.data.z > mag_max.z) ? dev.data.z : mag_max.z;
    }
    delay(100);
  }

  mag_offset.x = (mag_max.x + mag_min.x) / 2;
  mag_offset.y = (mag_max.y + mag_min.y) / 2;
  mag_offset.z = (mag_max.z + mag_min.z) / 2;
  bmm150_offset_save();

  Serial.printf("\n calibración finalizada \r\n");
  Serial.printf("mag_max.x: %.2f x_min: %.2f \t", mag_max.x, mag_min.x);
  Serial.printf("y_max: %.2f y_min: %.2f \t", mag_max.y, mag_min.y);
  Serial.printf("z_max: %.2f z_min: %.2f \r\n", mag_max.z, mag_min.z);
}

void handleMenu() {
  if (M5.BtnB.wasPressed()) {
    currentMenuOption = (MenuOption)((currentMenuOption + 1) % 3);  // Avanza a la siguiente opción
    drawMenu();
  }
  if (M5.BtnC.wasPressed()) {
    currentMenuOption = (MenuOption)((currentMenuOption + 2) % 3);  // Retrocede a la opción anterior
    drawMenu();
  }
  if (M5.BtnA.wasPressed()) {
    switch (currentMenuOption) {
      case MENU_CALIBRATION_TIME:
        calibrationTime += 5000;                              // Incrementa el tiempo de calibración en 5 segundos
        if (calibrationTime > 60000) calibrationTime = 5000;  // Límite máximo de 60 segundos
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setCursor(40, 120);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_PINK);
        M5.Lcd.printf("Tiempo: %d ms", calibrationTime);
        delay(1000);
        drawMenu();
        break;
      case MENU_CALIBRATE:
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setCursor(40, 120);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_PINK);
        M5.Lcd.print("Calibrando...");
        bmm150_calibrate(calibrationTime);
        isMenuActive = false;  // Vuelve a la brújula después de calibrar
        break;
      case MENU_EXIT:
        isMenuActive = false;  // Salir del menú
        break;
    }
  }
}

// Inicialización del sensor BMM150
int8_t bmm150_initialization() {
  int8_t rslt = BMM150_OK;

  dev.dev_id = 0x10;
  dev.intf = BMM150_I2C_INTF;
  dev.read = i2c_read;
  dev.write = i2c_write;
  dev.delay_ms = delay;

  mag_max.y = -2000;
  mag_max.z = -2000;
  mag_min.x = 2000;
  mag_min.y = 2000;
  mag_min.z = 2000;

  rslt = bmm150_init(&dev);
  dev.settings.pwr_mode = BMM150_NORMAL_MODE;
  rslt |= bmm150_set_op_mode(&dev);
  dev.settings.preset_mode = BMM150_PRESETMODE_ENHANCED;
  rslt |= bmm150_set_presetmode(&dev);

  return rslt;
}

// Carga los datos de calibración desde memoria
void bmm150_offset_load() {
  if (prefs.begin("bmm150", true)) {
    prefs.getBytes("offset", (uint8_t *)&mag_offset, sizeof(bmm150_mag_data));
    prefs.end();
    Serial.println("bmm150 load offset finish....");
  } else {
    Serial.println("bmm150 load offset failed....");
  }
}

void draw_compass_face(float heading) {
  // Limpiar el sprite y establecer el color de fondo
  compass_sprite.setColorDepth(16);  // Usa 16 bits para colores completos
  compass_sprite.createSprite(320, 240);
  compass_sprite.fillSprite(TFT_BLACK);

  // Centro de la brújula
  int center_x = 160;
  int center_y = 120;
  int radius = 80;
  int radius2 = 69;

  // Rellenar el círculo exterior
  compass_sprite.fillCircle(center_x, center_y, radius, TFT_WHITE);
  compass_sprite.fillCircle(center_x, center_y, radius2, TFT_BLACK);

  // Dibujar marcas de grados alrededor del círculo
  for (int angle = 0; angle < 360; angle += 30) {
    float rad = (angle + heading) * M_PI / 180.0;  // Ajustar el ángulo según el heading
    int x_outer = center_x + radius * cos(rad);
    int y_outer = center_y + radius * sin(rad);
    int x_inner = center_x + (radius - 10) * cos(rad);
    int y_inner = center_y + (radius - 10) * sin(rad);
    compass_sprite.drawLine(x_outer, y_outer, x_inner, y_inner, TFT_BLACK);

    // Añadir números
    float text_rad = (angle + heading - 90) * M_PI / 180.0;  // Ajustar el sistema de referencia
    int text_x = center_x + (radius + 15) * cos(text_rad);
    int text_y = center_y + (radius + 15) * sin(text_rad);
    String label = String(angle);  // Convertir el ángulo en texto
    compass_sprite.setTextColor(TFT_CYAN);
    compass_sprite.drawCentreString(label, text_x, text_y, 1);
  }

  // Añadir letras cardinales en puntos cardinales (N, E, S, O)
  float n_rad = (0 + heading - 90) * M_PI / 180.0;
  float e_rad = (90 + heading - 90) * M_PI / 180.0;
  float s_rad = (180 + heading - 90) * M_PI / 180.0;
  float o_rad = (270 + heading - 90) * M_PI / 180.0;

  compass_sprite.setTextColor(TFT_CYAN);
  compass_sprite.drawCentreString("N", center_x + (radius - 20) * cos(n_rad), center_y + (radius - 20) * sin(n_rad), 1);
  compass_sprite.drawCentreString("E", center_x + (radius - 20) * cos(e_rad), center_y + (radius - 20) * sin(e_rad), 1);
  compass_sprite.drawCentreString("S", center_x + (radius - 20) * cos(s_rad), center_y + (radius - 20) * sin(s_rad), 1);
  compass_sprite.drawCentreString("O", center_x + (radius - 20) * cos(o_rad), center_y + (radius - 20) * sin(o_rad), 1);
}

void draw_arrow() {
  // Centro de la brújula
  int center_x = 160;
  int center_y = 120;
  int arrow_length = 20;  // Longitud de la flecha

  // Calcula la posición de la punta de la flecha
  float tip_x = center_x;
  float tip_y = center_y - arrow_length;

  // Calcula las posiciones de los dos puntos de la base de la flecha
  float base_left_x = center_x - 10;
  float base_left_y = center_y;
  float base_right_x = center_x + 10;
  float base_right_y = center_y;

  // Dibujar la flecha
  compass_sprite.fillTriangle(tip_x, tip_y, base_left_x, base_left_y, base_right_x, base_right_y, TFT_PINK);
}

void setup() {
  M5.begin(true, false, true, false);  // Inicializa M5
  M5.Power.begin();                    // Inicializa el módulo de alimentación
  Wire.begin(21, 22, 400000UL);        // Configura la frecuencia de SDA SCL

  // Inicializar la pantalla
  M5.Lcd.fillScreen(TFT_BLACK);

  // Inicializar la brújula
  draw_compass_face(0);             // Dibuja la brújula inicialmente sin rotación
  draw_arrow();                     // Dibuja la flecha estática
  compass_sprite.pushSprite(0, 0);  // Mostrar la brújula y la flecha

  if (bmm150_initialization() != BMM150_OK) {
    compass_sprite.fillSprite(TFT_BLACK);
    compass_sprite.setTextColor(TFT_PINK);
    compass_sprite.setTextSize(1);
    compass_sprite.drawCentreString("BMM150 init failed", 160, 120, 2);
    compass_sprite.pushSprite(0, 0);  // Muestra mensaje de error
    for (;;) {
      delay(100);  // Retardo de 100ms
    }
  }

  bmm150_offset_load();

  // Inicializa los buffers circulares
  value_clear(&buffer_x);
  value_clear(&buffer_y);
  value_clear(&buffer_z);

  // Mostrar instrucciones iniciales
  M5.Lcd.setTextColor(TFT_PINK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.drawCentreString("Pulsar BtnA para calibrar", 160, 220, 2);
  M5.Lcd.drawCentreString("BtnB: + Avg", 160, 200, 1);
  M5.Lcd.drawCentreString("BtnC: - Avg", 160, 210, 1);
}

void loop() {
  char text_string[100];
  M5.update();  // Lee el estado de los botones

  if (isMenuActive) {
    handleMenu();
    return;
  }

  // Ajuste de AVERAGE_COUNT utilizando botones
  if (M5.BtnB.wasPressed()) {  // Incrementar AVERAGE_COUNT
    AVERAGE_COUNT++;
    if (AVERAGE_COUNT > CIRCULAR_BUFFER_LEN) AVERAGE_COUNT = CIRCULAR_BUFFER_LEN;
    Serial.printf("Promediado incrementado a %d\n", AVERAGE_COUNT);
  }

  if (M5.BtnC.wasPressed()) {  // Decrementar AVERAGE_COUNT
    AVERAGE_COUNT--;
    if (AVERAGE_COUNT < 1) AVERAGE_COUNT = 1;
    Serial.printf("Promediado decrementado a %d\n", AVERAGE_COUNT);
  }

  // Botón A
  if (M5.BtnA.wasPressed()) {
    isMenuActive = true;
    drawMenu();
    return;
  }

  // Lectura de datos del magnetómetro
  bmm150_read_mag_data(&dev);

  // Añade los valores a los buffers circulares
  value_queue(&buffer_x, (float)(dev.data.x - mag_offset.x));
  value_queue(&buffer_y, (float)(dev.data.y - mag_offset.y));
  value_queue(&buffer_z, (float)(dev.data.z - mag_offset.z));

  // Calcula los valores promediados
  float avg_x = value_average(&buffer_x, AVERAGE_COUNT);
  float avg_y = value_average(&buffer_y, AVERAGE_COUNT);
  float avg_z = value_average(&buffer_z, AVERAGE_COUNT);

  // Calcula la dirección de la cabeza usando los valores promediados
  float head_dir = atan2(avg_x, avg_y) * 180.0 / M_PI;
  if (head_dir < 0) head_dir += 360.0;  // Asegura que el ángulo esté entre 0 y 360 grados

  // Invertir el ángulo para que coincida con la dirección correcta
  float corrected_head_dir = 360.0 - head_dir;
  if (corrected_head_dir >= 360.0) corrected_head_dir -= 360.0;

  Serial.printf("Ángulo de dirección: %.2f\n", corrected_head_dir);
  Serial.printf("AVG MAG X : %.2f \t AVG MAG Y : %.2f \t AVG MAG Z : %.2f \n", avg_x, avg_y, avg_z);
  Serial.printf("Compensación eje X : %.2f \t Compensación eje Y : %.2f \t Compensación eje Z : %.2f \n", mag_offset.x, mag_offset.y, mag_offset.z);

  // Actualiza la brújula con la nueva dirección
  draw_compass_face(head_dir);      // Redibuja la brújula con la rotación
  draw_arrow();                     // Dibuja la flecha estática
  compass_sprite.pushSprite(0, 0);  // Actualizar la pantalla con la brújula y la flecha

  // Mostrar valores promediados y configuración
  M5.Lcd.setTextColor(TFT_PINK);
  M5.Lcd.setTextSize(1);
  sprintf(text_string, "Valor promedio: %d", AVERAGE_COUNT);
  M5.Lcd.drawString(text_string, 10, 225, 1);  // Muestra el número de promediado

  // Mostrar el rumbo en grados en la esquina inferior derecha
  sprintf(text_string, "%.1f\xA7", corrected_head_dir);
  // Calcular la longitud del texto para centrarlo correctamente
  int text_width = strlen(text_string) * 6;  // Aproximadamente 6 píxeles por carácter en tamaño 1
  int x_position = 320 - text_width - 5;     // 5 píxeles de margen desde el borde
  int y_position = 225;                      // Posición Y cerca del borde inferior
  M5.Lcd.setTextColor(TFT_PINK);
  M5.Lcd.drawString(text_string, x_position, y_position, 1);

  delay(10);  // Retardo reducido a 10ms para mayor frecuencia de muestreo
}