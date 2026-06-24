# Manual de Instalación: Softproductiva IPTV Gateway

Este documento detalla los pasos para realizar una instalación limpia, compilar, configurar y ejecutar la aplicación **Softproductiva IPTV Gateway** en cualquier servidor Linux (Ubuntu/Debian recomendado) utilizando el repositorio Git.

---

## 1. Requisitos del Sistema

Instala las dependencias y compiladores requeridos en tu servidor ejecutando:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config ffmpeg \
                    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev git
```

---

## 2. Preparación del Directorio de Instalación

El directorio estándar de instalación del servicio es `/opt/softproductiva_iptv`. Ejecuta los siguientes comandos para crear la carpeta y asignarle permisos a tu usuario:

```bash
# Crear directorio principal
sudo mkdir -p /opt/softproductiva_iptv

# Asignar propiedad al usuario actual (reemplaza $USER por tu nombre de usuario si es necesario)
sudo chown -R $USER:$USER /opt/softproductiva_iptv
```

---

## 3. Clonación del Repositorio Git

Utiliza el siguiente comando para clonar el código fuente directamente en el directorio destino (reemplaza `<TOKEN_DE_GITHUB>` con tu token de acceso personal si el repositorio es privado):

```bash
git clone https://<TOKEN_DE_GITHUB>@github.com/correorefacil-coder/iptv-motor-core.git /opt/softproductiva_iptv
```

Ingresa al directorio del proyecto:

```bash
cd /opt/softproductiva_iptv
```

---

## 4. Compilación del Proyecto

Ejecuta la configuración y el proceso de compilación para generar el binario `softproductiva_iptv`:

```bash
# Configurar el directorio de construcción
cmake -B build

# Compilar usando todos los núcleos disponibles
cmake --build build -j$(nproc)
```

Al terminar, el ejecutable estará en:
`/opt/softproductiva_iptv/build/softproductiva_iptv`

---

## 5. Base de Datos de Usuarios y Roles

Los siguientes usuarios y roles ya vienen preconfigurados en el archivo `/opt/softproductiva_iptv/users.json`. Úsalos para iniciar sesión en la interfaz web:

| Rol | Usuario | Contraseña | Permisos |
| :--- | :--- | :--- | :--- |
| **SuperAdmin** | `Cristian` | `cristian123` | Control total del sistema, administración de usuarios. |
| **SuperAdmin** | `IngCristian` | `CARE90po#` | Control total del sistema, administración de usuarios. |
| **Admin** | `admin` | `admin123` | Administración completa de canales e interfaces de red. |
| **Consulta** | `consultas` | `consultas123` | Solo lectura (Dashboard). No puede modificar ni agregar canales. |
| **Programadores** | `cafeteria` | `cafeteria` | Solo puede activar/desactivar y cambiar el archivo de video de su canal asignado (`Cafetería`). |
| **Programadores** | `comunicaciones` | `comunicaciones` | Solo puede activar/desactivar y cambiar el archivo de video de sus canales asignados (`Institucional`). |

---

## 6. Ejecución del Servicio

### Prueba en Consola
Ejecuta el binario indicando el puerto de escucha y la ruta al archivo de configuración:

```bash
/opt/softproductiva_iptv/build/softproductiva_iptv 45020 /opt/softproductiva_iptv/config.json
```

---

## 7. Configuración como Servicio del Sistema (Systemd)

Sigue estos pasos detallados para configurar el inicio automático con el servidor:

1.  Crea el archivo de configuración del servicio en systemd:
    ```bash
    sudo nano /etc/systemd/system/softproductiva_iptv.service
    ```

2.  Copia y pega el siguiente bloque en el editor (reemplaza `User=cristian` con el usuario que correrá el proceso en tu máquina o usa `root`):
    ```ini
    [Unit]
    Description=Softproductiva IPTV Gateway Service
    After=network.target

    [Service]
    Type=simple
    User=cristian
    WorkingDirectory=/opt/softproductiva_iptv
    ExecStart=/opt/softproductiva_iptv/build/softproductiva_iptv 45020 /opt/softproductiva_iptv/config.json
    Restart=always
    RestartSec=5
    StandardOutput=journal
    StandardError=journal

    [Install]
    WantedBy=multi-user.target
    ```

3.  Habilita e inicia el servicio:
    ```bash
    # Recargar daemon de servicios
    sudo systemctl daemon-reload

    # Habilitar para arranque automático
    sudo systemctl enable softproductiva_iptv.service

    # Iniciar servicio inmediatamente
    sudo systemctl start softproductiva_iptv.service
    ```

4.  Comandos útiles de diagnóstico:
    ```bash
    # Ver estado actual
    sudo systemctl status softproductiva_iptv.service

    # Ver logs del servicio en tiempo real
    sudo journalctl -u softproductiva_iptv.service -f
    ```

---

## 8. Notas Importantes

*   **Permisos de Escritura**: El servicio genera segmentos dinámicos de HLS en `/opt/softproductiva_iptv/www/hls/`. Asegúrate de que el usuario especificado en `User=` de la unidad systemd tenga permisos de escritura en dicha carpeta.
*   **Subidas de Videos**: Si utilizas VideoPacks locales, los videos deben colocarse en `/opt/softproductiva_iptv/uploads/`.
