#!/bin/bash

# --- CONFIGURATION ---
# Direct links to your v1.0.1 and v1.0.2 release
BACKEND_URL="https://github.com/evalyx/volmix/releases/download/v1.0.2/volmix_backend"
GUI_URL="https://github.com/evalyx/volmix/releases/download/v1.0.1/VolMixGUI"

BIN_PATH="/usr/local/bin"
DESKTOP_PATH="$HOME/.local/share/applications"
SYSTEMD_PATH="$HOME/.config/systemd/user"
CONFIG_PATH="$HOME/.config/volmix"

# --- PRE-INSTALL CHECKS ---
# 1. Check for WirePlumber
if ! command -v wpctl &> /dev/null; then
    echo "--------------------------------------------------------"
    echo "ERROR: 'wpctl' (WirePlumber) not found!"
    echo "VolMix requires WirePlumber to control audio volumes."
    echo "Please install it first (e.g., sudo pacman -S wireplumber)."
    echo "--------------------------------------------------------"
    exit 1
fi

# 2. Check for curl
if ! command -v curl &> /dev/null; then
    echo "ERROR: 'curl' is required to download the binaries."
    exit 1
fi

# --- INSTALLATION ---

# 1. Create temporary download directory
echo "Preparing download workspace..."
mkdir -p /tmp/volmix_install

# 2. Download Binaries
echo "Downloading VolMix v1.0.1 and v1.0.2 from GitHub..."
curl -L "$BACKEND_URL" -o /tmp/volmix_install/volmix-backend
curl -L "$GUI_URL" -o /tmp/volmix_install/volmix-gui

# 3. Create System Directories
mkdir -p "$DESKTOP_PATH" "$SYSTEMD_PATH" "$CONFIG_PATH"

# 4. Move Binaries to Path
echo "Installing binaries to $BIN_PATH..."
sudo mv /tmp/volmix_install/volmix-backend "$BIN_PATH/volmix-backend"
sudo mv /tmp/volmix_install/volmix-gui "$BIN_PATH/volmix-gui"

# 5. Set Permissions
sudo chmod +x "$BIN_PATH/volmix-backend" "$BIN_PATH/volmix-gui"

# 6. Create the Desktop Entry
echo "Creating desktop entry..."
cat <<EOF > "$DESKTOP_PATH/volmix.desktop"
[Desktop Entry]
Name=VolMix Matrix
Comment=Audio Fader Configuration
Exec=volmix-gui
Icon=audio-card
Terminal=false
Type=Application
Categories=AudioVideo;Audio;
EOF

# 7. Create the Systemd User Service
echo "Setting up systemd background service..."
cat <<EOF > "$SYSTEMD_PATH/volmix.service"
[Unit]
Description=VolMix C++ Backend Service
After=pipewire.service
Requires=pipewire.service

[Service]
ExecStart=$BIN_PATH/volmix-backend
WorkingDirectory=%h/.config/volmix
Environment=XDG_RUNTIME_DIR=/run/user/%U
Environment=HOME=%h
StandardInput=null
StandardOutput=journal
StandardError=journal
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
EOF

# 8. Enable and Start the service
echo "Starting service..."
systemctl --user daemon-reload
systemctl --user enable volmix.service
systemctl --user restart volmix.service

# 9. Cleanup
rm -rf /tmp/volmix_install

echo "--------------------------------------------------------"
echo "INSTALLATION SUCCESSFUL!"
echo "Version: v1.0.1"
echo "Config Folder: $CONFIG_PATH"
echo "--------------------------------------------------------"
echo "You can now launch 'VolMix Matrix' from your app menu."
