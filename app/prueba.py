import lvgl as lv
import time
from machine import Pin, I2C
import lcd_bus
import st7789
import task_handler
import machine
try:
    import uhashlib as hashlib_mod  # MicroPython
except ImportError:
    import hashlib as hashlib_mod   # CPython fallback for dev

# Compatibilidad de const para entornos que no sean MicroPython
try:
    from micropython import const
except Exception:
    def const(x):
        return x

# Importar el controlador CST816S compartido (con fallback para entorno de desarrollo)
try:
    from cst816s_2 import CST816S  # internal library
except Exception:
    CST816S = None
# from cst816s import _MotionMask, _EnConLR, _EnConUD, _EnDClick


# Configuración de pines para la pantalla
_WIDTH = 240
_HEIGHT = 320
_BL = 1       # Pin de retroiluminación
_RST = -1     # Pin de reset, -1 si no se usa
_DC = 42      # Pin DC
_MOSI = 38    # Pin MOSI
_MISO = 40    # Pin MISO
_SCK = 39     # Pin SCK
_HOST = 2     # SPI2
_LCD_CS = 45  # Pin CS para LCD
_LCD_FREQ = 40000000  # Frecuencia SPI para LCD

# Configuración de pines para el touch (I2C)
_TP_SDA = 48  # Pin SDA para touch
_TP_SCL = 47  # Pin SCL para touch

# Configurar bus SPI
spi_bus = machine.SPI.Bus(
    host=_HOST,
    mosi=_MOSI,
    miso=_MISO,
    sck=_SCK
)

# Configurar bus LCD
display_bus = lcd_bus.SPIBus(
    spi_bus=spi_bus,
    freq=_LCD_FREQ,
    dc=_DC,
    cs=_LCD_CS,
)

# Inicializar LVGL
lv.init()

# Crear el display
display = st7789.ST7789(
    data_bus=display_bus,
    display_width=_WIDTH,
    display_height=_HEIGHT,
    backlight_pin=_BL,
    color_space=lv.COLOR_FORMAT.RGB565,
    color_byte_order=st7789.BYTE_ORDER_RGB,
    rgb565_byte_swap=True,
)

# Encender y configurar el display
display.set_power(True)
display.init()
display.set_backlight(80)  # Brillo al 80%

# Configurar el I2C para el touch
print("Inicializando I2C para el touch...")
i2c = I2C(1, scl=Pin(_TP_SCL), sda=Pin(_TP_SDA), freq=100000)

# Escanear dispositivos I2C
print("Escaneando dispositivos I2C...")
devices = i2c.scan()
print(f"Dispositivos encontrados: {[hex(d) for d in devices]}")

# Inicializar el touch con el controlador CST816S
print("Inicializando controlador táctil CST816S...")
touch = None
try:
    # Inicializamos el controlador según la nueva implementación
    touch = CST816S(i2c)
    # Configuramos para desactivar el modo de auto-sleep
    touch.auto_sleep = False
    # ===============================
    _MotionMask = const(0xEC)

    # Enables continuous left and right sliding
    _EnConLR = const(0x04)
    # Enables continuous up and down sliding
    _EnConUD = const(0x02)
    # Enable double-click action
    _EnDClick = const(0x01)
# ===============================
    print("Controlador táctil inicializado correctamente")
    # Habilitar deslizamientos continuos horizontales y verticales
    touch._write_reg(_MotionMask, _EnConLR | _EnConUD | _EnDClick)

    # Configurar parámetros de sensibilidad para mejorar la detección de gestos
    touch.wake_up_threshold = 200  # Más sensible (1-255)
    touch.wake_up_scan_frequency = 200  # Más sensible (1-255)

    # Establecer un tiempo de auto-sleep más largo si es necesario
    touch.auto_sleep_timeout = 10  # 10 segundos
except Exception as e:
    print(f"Error al inicializar el controlador táctil: {e}")

# Inicializar el gestor de tareas
th = task_handler.TaskHandler()
if hasattr(th, 'enable'):
    th.enable()
    print("TaskHandler activado")

# Utilidades SHA-256
def _to_hex(b):
    """Convierte bytes a hex en puro Python (sin ubinascii/hexlify)."""
    return ''.join('{:02x}'.format(x) for x in b)

def sha256_hex(data):
    """Devuelve el SHA-256 en hexadecimal usando uhashlib."""
    h = hashlib_mod.sha256(data)
    return _to_hex(h.digest())

def wrap_hex(s, width=16):
    """Envuelve una cadena hex en varias líneas de ancho fijo para mejor legibilidad."""
    return "\n".join(s[i:i+width] for i in range(0, len(s), width))

# Función para crear la tabla con scroll nativo de LVGL
def create_table_with_native_scroll():
    # Obtener la pantalla activa
    scr = lv.screen_active()
    
    # Fondo
    scr.set_style_bg_color(lv.color_hex(0x2E1E1E), 0)
    
    # Panel superior
    header = lv.obj(scr)
    header.set_size(240, 50)
    header.align(lv.ALIGN.TOP_MID, 0, 0)
    header.set_style_bg_color(lv.color_hex(0x251818), 0)
    header.set_style_radius(0, 0)
    
    # Título
    titulo = lv.label(header)
    titulo.set_text("Tabla con Scroll Nativo")
    titulo.align(lv.ALIGN.CENTER, 0, 0)
    titulo.set_style_text_color(lv.color_hex(0xf4D6cd), 0)
    
    # Crear el contenedor para la tabla con scroll nativo
    panel = lv.obj(scr)
    panel.set_size(220, 200)
    panel.align(lv.ALIGN.TOP_MID, 0, 70)
    panel.set_style_bg_color(lv.color_hex(0x443231), 0)
    panel.set_style_radius(8, 0)
    
    # Configuración crucial para scroll nativo
    panel.add_flag(lv.obj.FLAG.SCROLLABLE)
    panel.set_scroll_dir(lv.DIR.ALL)  # Permitir scroll en todas direcciones
    panel.set_scrollbar_mode(lv.SCROLLBAR_MODE.ON)
    panel.add_flag(lv.obj.FLAG.SCROLL_MOMENTUM)
    
    # Crear la tabla dentro del panel
    table = lv.table(panel)
    
    # Configurar columnas y filas
    table.set_column_count(3)
    table.set_row_count(30)
    # Anchos de columna
    table.set_column_width(0, 120)
    table.set_column_width(1, 80)
    table.set_column_width(2, 80)
    
    # Llenar cabecera
    table.set_cell_value(0, 0, "Producto")
    table.set_cell_value(0, 1, "Precio")
    table.set_cell_value(0, 2, "Cantidad")
    
    # Algunos builds de LVGL para MicroPython no exponen add_cell_ctrl.
    # Aplicar estilo a la cabecera usando PART.HEADER cuando esté disponible,
    # y si no hacer un fallback a ITEMS (menos preciso).
    header_style = lv.style_t()
    header_style.init()
    header_style.set_bg_color(lv.color_hex(0xf6bb5c))
    header_style.set_text_color(lv.color_hex(0xFFFFFF))
    try:
        # Preferir aplicar estilo al PART.HEADER si la versión de LVGL lo soporta
        table.add_style(header_style, lv.PART.HEADER)
    except Exception:
        # Fallback: aplicar al conjunto de ITEMS (puede afectar a todas las filas)
        table.add_style(header_style, lv.PART.ITEMS)
    
    # Datos de productos
    productos = [
        "Manzana", "Banana", "Limón", "Uva", "Melón", 
        "Durazno", "Nueces", "Fresa", "Naranja", "Pera",
        "Sandía", "Piña", "Mango", "Cereza", "Kiwi",
        "Coco", "Papaya", "Higo", "Mandarina", "Arándano",
        "Frambuesa", "Mora", "Granada", "Maracuyá", "Guayaba",
        "Lichi", "Caqui", "Ciruela", "Pomelo", "Aguacate"
    ]
    
    precios = [
        "$7", "$4", "$6", "$2", "$5", 
        "$1", "$9", "$3", "$5", "$4",
        "$8", "$6", "$7", "$5", "$3",
        "$10", "$5", "$4", "$3", "$7",
        "$6", "$5", "$8", "$9", "$4",
        "$7", "$3", "$4", "$5", "$8"
    ]
    
    cantidades = [
        "12", "25", "8", "30", "5", 
        "10", "15", "20", "7", "14",
        "3", "9", "18", "24", "11",
        "6", "13", "19", "22", "16",
        "4", "27", "10", "8", "15",
        "21", "9", "17", "23", "6"
    ]
    
    # Llenar la tabla con datos
    for i in range(len(productos)):
        table.set_cell_value(i + 1, 0, productos[i])
        table.set_cell_value(i + 1, 1, precios[i])
        table.set_cell_value(i + 1, 2, cantidades[i])
    
    # Hacer la tabla más ancha que el contenedor para forzar scroll horizontal
    table.set_width(280)
    
    # Crear el contenedor para la tabla con scroll nativo
    footer = lv.obj(scr)
    footer.set_size(240, 50)
    footer.align(lv.ALIGN.TOP_MID, 0, 270)
    footer.set_style_bg_color(lv.color_hex(0x443231), 0)
    footer.set_style_radius(8, 0)
    
    # Configuración crucial para scroll nativo
    footer.add_flag(lv.obj.FLAG.SCROLLABLE)
    footer.set_scroll_dir(lv.DIR.HOR)  # Permitir scroll horizontal
    footer.set_scrollbar_mode(lv.SCROLLBAR_MODE.ON)
    footer.add_flag(lv.obj.FLAG.SCROLL_MOMENTUM)

    # Instrucciones
    label = lv.label(footer)
    label.set_text("Prueba scroll nativo")
    label.align(lv.ALIGN.TOP_MID, 0, 0)
    label.set_style_text_color(lv.color_hex(0xCDD6F4), 0)
    
    # Mostrar información de debug
    debug_label = lv.label(footer)
    debug_label.set_text("Ver consola para eventos")
    debug_label.align(lv.ALIGN.TOP_MID, 0, 10)
    debug_label.set_style_text_color(lv.color_hex(0xAAAAAA), 0)
    # Evitar saltos de línea automáticos: que recorte o desplace en una sola línea
    debug_label.set_long_mode(lv.label.LONG_MODE.SCROLL)
    
    return panel, table, debug_label

# Contador para estadísticas
stats = {
    "touch_events": 0,
    "gestures": 0
}

# Crear la tabla
panel, table, debug_label = create_table_with_native_scroll()

# Etiqueta para mostrar el SHA-256 en pantalla
#hash_label = lv.label(lv.screen_active())
#hash_label.set_text("SHA256: esperando...")
#hash_label.align(lv.ALIGN.BOTTOM_MID, 0, -60)
#hash_label.set_style_text_color(lv.color_hex(0x99E2B4), 0)

print("Tabla creada con scroll nativo. Prueba desplazando con el dedo.")
print("Iniciando bucle principal...")

# Bucle principal
while True:
    try:
        if touch:
            # Verificar si hay un toque
            coords = touch._get_coords()
            
            if coords:
                state, x, y = coords
                stats["touch_events"] += 1
                
                # Mostrar coordenadas cada 10 eventos para no saturar
                if stats["touch_events"] % 10 == 0:
                    print(f"Toque detectado en ({x}, {y})")
                    
                # Actualizar etiqueta de debug cada 50 eventos
                if stats["touch_events"] % 50 == 0:
                    # Calcular y mostrar un SHA-256 en una sola línea
                    try:
                        payload = f"{x},{y},{stats['touch_events']}".encode()
                        hhex = sha256_hex(payload)
                        debug_label.set_text(f"Toques: {stats['touch_events']} | SHA256: {hhex}")
                    except Exception as e:
                        debug_label.set_text(f"Toques: {stats['touch_events']} | SHA256 error: {e}")
        
        # Pausa para no saturar el sistema
        time.sleep_ms(30)
    except Exception as e:
        print(f"Error en el bucle: {e}")
        time.sleep_ms(1000)