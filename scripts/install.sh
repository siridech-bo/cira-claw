#!/bin/bash
#
# CiRA CLAW Installation Script
#
# This script:
# 1. Builds the project
# 2. Creates a 'cira' system user
# 3. Copies files to /opt/cira-claw
# 4. Installs the systemd service
# 5. Enables and optionally starts the service
#
# Usage: sudo ./scripts/install.sh [--no-start]
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
INSTALL_DIR="/opt/cira-claw"
CIRA_USER="cira"
CIRA_GROUP="cira"
CIRA_HOME="/home/cira"
SERVICE_FILE="cira-claw.service"
START_SERVICE=true

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --no-start)
            START_SERVICE=false
            shift
            ;;
        --help|-h)
            echo "Usage: sudo ./scripts/install.sh [--no-start]"
            echo ""
            echo "Options:"
            echo "  --no-start    Install but don't start the service"
            echo "  --help, -h    Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    exit 1
fi

# Get the directory where the script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${GREEN}=== CiRA CLAW Installation ===${NC}"
echo ""
echo "Project directory: $PROJECT_DIR"
echo "Install directory: $INSTALL_DIR"
echo ""

# Step 1: Check Node.js is installed
echo -e "${YELLOW}[1/7] Checking Node.js...${NC}"
if ! command -v node &> /dev/null; then
    echo -e "${RED}Error: Node.js is not installed${NC}"
    echo "Please install Node.js 20+ first:"
    echo "  curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -"
    echo "  sudo apt-get install -y nodejs"
    exit 1
fi

NODE_VERSION=$(node --version | cut -d'v' -f2 | cut -d'.' -f1)
if [ "$NODE_VERSION" -lt 20 ]; then
    echo -e "${RED}Error: Node.js 20+ required, found $(node --version)${NC}"
    exit 1
fi
echo "Found Node.js $(node --version)"

# Step 2: Build the project
echo -e "${YELLOW}[2/7] Building project...${NC}"
cd "$PROJECT_DIR"

if [ ! -d "node_modules" ]; then
    echo "Installing dependencies..."
    npm install
fi

echo "Compiling TypeScript..."
npm run build

# Build dashboard if it exists
if [ -d "dashboard" ] && [ -f "dashboard/package.json" ]; then
    echo "Building dashboard..."
    cd dashboard
    if [ ! -d "node_modules" ]; then
        npm install
    fi
    npm run build
    cd "$PROJECT_DIR"
fi

# Step 3: Create cira user and group
echo -e "${YELLOW}[3/7] Creating system user...${NC}"
if ! getent group "$CIRA_GROUP" > /dev/null 2>&1; then
    groupadd --system "$CIRA_GROUP"
    echo "Created group: $CIRA_GROUP"
else
    echo "Group $CIRA_GROUP already exists"
fi

if ! id "$CIRA_USER" > /dev/null 2>&1; then
    useradd --system \
        --gid "$CIRA_GROUP" \
        --home-dir "$CIRA_HOME" \
        --create-home \
        --shell /bin/false \
        "$CIRA_USER"
    echo "Created user: $CIRA_USER"
else
    echo "User $CIRA_USER already exists"
fi

# Step 4: Create installation directory and copy files
echo -e "${YELLOW}[4/7] Installing files...${NC}"
mkdir -p "$INSTALL_DIR"

# Copy built files
cp -r dist "$INSTALL_DIR/"
cp -r node_modules "$INSTALL_DIR/"
cp package.json "$INSTALL_DIR/"

# Copy dashboard if built
if [ -d "dashboard/dist" ]; then
    mkdir -p "$INSTALL_DIR/dashboard"
    cp -r dashboard/dist "$INSTALL_DIR/dashboard/"
fi

# Copy workspace templates
if [ -d "workspace" ]; then
    cp -r workspace "$INSTALL_DIR/"
fi

# Set ownership
chown -R "$CIRA_USER:$CIRA_GROUP" "$INSTALL_DIR"
echo "Files installed to $INSTALL_DIR"

# Step 5: Setup cira home directory
echo -e "${YELLOW}[5/7] Setting up configuration...${NC}"
CIRA_CONFIG_DIR="$CIRA_HOME/.cira"
mkdir -p "$CIRA_CONFIG_DIR"
mkdir -p "$CIRA_CONFIG_DIR/workspace/skills"
mkdir -p "$CIRA_CONFIG_DIR/workspace/models"
mkdir -p "$CIRA_CONFIG_DIR/nodes"
mkdir -p "$CIRA_CONFIG_DIR/credentials"
mkdir -p "$CIRA_CONFIG_DIR/logs"

# Copy default configuration if it doesn't exist
if [ ! -f "$CIRA_CONFIG_DIR/cira.json" ]; then
    if [ -f "$PROJECT_DIR/cira.example.json" ]; then
        cp "$PROJECT_DIR/cira.example.json" "$CIRA_CONFIG_DIR/cira.json"
        echo "Created default configuration"
    fi
fi

# Copy workspace templates if they don't exist
if [ ! -f "$CIRA_CONFIG_DIR/workspace/AGENTS.md" ] && [ -f "$INSTALL_DIR/workspace/AGENTS.md" ]; then
    cp -r "$INSTALL_DIR/workspace/"* "$CIRA_CONFIG_DIR/workspace/"
    echo "Copied workspace templates"
fi

# Set ownership of config directory
chown -R "$CIRA_USER:$CIRA_GROUP" "$CIRA_CONFIG_DIR"
chmod 700 "$CIRA_CONFIG_DIR/credentials"
echo "Configuration directory: $CIRA_CONFIG_DIR"

# Step 6: Install systemd service
echo -e "${YELLOW}[6/7] Installing systemd service...${NC}"
if [ -f "$PROJECT_DIR/$SERVICE_FILE" ]; then
    cp "$PROJECT_DIR/$SERVICE_FILE" /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable cira-claw.service
    echo "Service installed and enabled"
else
    echo -e "${RED}Warning: $SERVICE_FILE not found, skipping service installation${NC}"
fi

# Step 7: Start service (optional)
echo -e "${YELLOW}[7/7] Starting service...${NC}"
if [ "$START_SERVICE" = true ]; then
    systemctl start cira-claw.service
    sleep 2

    if systemctl is-active --quiet cira-claw.service; then
        echo -e "${GREEN}Service started successfully${NC}"
    else
        echo -e "${RED}Service failed to start. Check logs with: journalctl -u cira-claw -f${NC}"
        exit 1
    fi
else
    echo "Service not started (--no-start specified)"
    echo "Start manually with: sudo systemctl start cira-claw"
fi

# Done
echo ""
echo -e "${GREEN}=== Installation Complete ===${NC}"
echo ""
echo "CiRA CLAW is installed!"
echo ""
echo "Useful commands:"
echo "  sudo systemctl status cira-claw    # Check status"
echo "  sudo systemctl restart cira-claw   # Restart service"
echo "  sudo systemctl stop cira-claw      # Stop service"
echo "  sudo journalctl -u cira-claw -f    # View logs"
echo "  sudo systemctl reload cira-claw    # Reload config (SIGHUP)"
echo ""
echo "Configuration: $CIRA_CONFIG_DIR/cira.json"
echo "Dashboard: http://localhost:18790"
echo ""
echo "To configure the AI agent, set your Anthropic API key:"
echo "  echo '{\"api_key\": \"your-key-here\"}' | sudo -u cira tee $CIRA_CONFIG_DIR/credentials/claude.json"
echo ""
