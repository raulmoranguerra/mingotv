# mingotv

# Simpsons TV – Raspberry Pi Zero 2 W

Este proyecto implementa una “televisión Simpsons” autónoma basada en una **Raspberry Pi Zero 2 W**, una **pantalla Waveshare DPI con panel táctil capacitivo** y un **amplificador PAM8302A**, diseñada para reproducir episodios de forma continua con una experiencia similar a una televisión analógica antigua.

El sistema está pensado como un **appliance embebido**:
- Arranque rápido
- Sin entorno gráfico
- Sin interacción por teclado/ratón
- Controlado únicamente por hardware físico y pantalla táctil
- Funcionamiento estable a largo plazo

---

## Objetivos del proyecto

- Sistema minimalista y reproducible
- Evitar dependencias innecesarias (X11, Wayland, systemd)
- Control total del hardware (GPIO, framebuffer, audio)
- Comportamiento determinista
- Código y lógica comprensibles y mantenibles

---

## Funcionalidad final

### Interruptor físico (GPIO26)
- **ON**
  - Enciende el backlight
  - Inicia la reproducción de un episodio aleatorio
- **OFF**
  - Detiene reproducción (vídeo y audio)
  - Limpia el framebuffer
  - Apaga el backlight

### Pantalla táctil
- **Tap simple**
  - Pausa / reanuda el episodio actual
- **Doble tap**
  - Transición de ruido estático
  - Cambio a un nuevo episodio aleatorio
- Durante la pausa:
  - No es posible cambiar de episodio

### Reproducción
- Episodios almacenados en `/mnt/videos`
- Selección aleatoria
- Salida de vídeo directa a framebuffer
- Audio por ALSA
- Transición visual tipo “static noise” entre episodios

---

## Arquitectura

### Hardware
- Raspberry Pi Zero 2 W
- Pantalla Waveshare DPI 640×480 con táctil Goodix
- Amplificador PAM8302A
- Audio PWM
- Backlight controlado por GPIO
- Interruptor ON/OFF entre GPIO26 y GND

### Software
- Buildroot
- Kernel oficial Raspberry Pi
- BusyBox init
- FFmpeg (reproducción de vídeo y audio)
- Daemon propio en C (`tvd`)
- Sin aceleración gráfica ni entorno de ventanas

---

## Diseño del sistema

### Arranque
- Boot optimizado (~5 segundos)
- Consola únicamente por USB OTG (`ttyGS0`)
- Eliminación de mensajes innecesarios
- BusyBox init con scripts mínimos

### Almacenamiento de vídeos
- Uso de una tercera partición en la microSD
- Inicialización automática en primer arranque
- Formateo ext4
- Montaje persistente en `/mnt/videos`

### Vídeo
- Salida directa a `/dev/fb0`
- Resolución fija (640×480)
- Limpieza manual del framebuffer cuando es necesario
- Sin uso de DRM, KMS ni OpenGL

### Audio
- Driver `snd_bcm2835`
- Salida PWM
- Control por ALSA
- Compatible con amplificador PAM8302A

### GPIO
- Backlight controlado por GPIO18
- Audio PWM en GPIO19 (ALT5)
- Interruptor físico en GPIO26 con pull-up interno
- Configuración aplicada en arranque

### Pantalla táctil
- Lectura directa desde `/dev/input/event0`
- Procesado de eventos evdev
- Detección de:
  - Tap simple
  - Doble tap
- Gestión temporal para distinguir gestos

---

## Daemon principal (`tvd`)

El núcleo del sistema es un daemon en C que:

- Supervisa continuamente:
  - Estado del interruptor
  - Eventos táctiles
- Gestiona los estados:
  - OFF
  - PLAYING
  - PAUSED
- Lanza y detiene la reproducción de forma segura
- Evita múltiples instancias simultáneas
- Gestiona correctamente señales y procesos
- Controla el backlight y el framebuffer

El uso de C (en lugar de shell) fue clave para:
- Eliminar condiciones de carrera
- Evitar procesos huérfanos
- Manejar correctamente eventos concurrentes
- Mejorar estabilidad general

---

## Ruido estático

La transición entre episodios incluye un efecto de “ruido estático” implementado mediante:
- Escritura directa de datos aleatorios en el framebuffer
- Duración controlada
- Uso exclusivo durante cambios de episodio (no en pausa)

El efecto es visualmente similar a una televisión analógica sin señal.

---
