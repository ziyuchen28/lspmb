
BUILD_DIR := build
TOOLS_DIR := .tools

-include .env

# test 
T ?= .*

.PHONY: all config build clean 

all: config build

vendor-add:
	@echo "==> Synchronizing Git submodules..."
	git submodule update --init --recursive --force


vendor-update: 
	@echo "==> Updating vendor..."
	git submodule update --remote


config:
	@echo "==> Generating build config..."
	cmake -S . -B $(BUILD_DIR) 


build: config
	@echo "==> Building project..."
	cmake --build $(BUILD_DIR) -j -- --no-print-directory


install-jre:
	@echo "==> Installing hermetic Java 21 JRE..."
	@mkdir -p $(TOOLS_DIR)
	@if [ ! -x "$(TOOLS_DIR)/jre/bin/java" ]; then \
		set -e; \
		echo "Detecting OS and Architecture..."; \
		OS=$$(uname -s | tr '[:upper:]' '[:lower:]'); \
		ARCH=$$(uname -m); \
		if [ "$$OS" = "darwin" ]; then API_OS="mac"; else API_OS="linux"; fi; \
		if [ "$$ARCH" = "x86_64" ]; then API_ARCH="x64"; \
		elif [ "$$ARCH" = "arm64" ] || [ "$$ARCH" = "aarch64" ]; then API_ARCH="aarch64"; \
		else echo "Unsupported architecture: $$ARCH"; exit 1; fi; \
		URL="https://api.adoptium.net/v3/binary/latest/21/ga/$${API_OS}/$${API_ARCH}/jre/hotspot/normal/eclipse?project=jdk"; \
		echo "Downloading Eclipse Temurin JRE 21 ($${API_OS}-$${API_ARCH})..."; \
		curl -L -f "$$URL" -o $(TOOLS_DIR)/jre.tar.gz; \
		mkdir -p $(TOOLS_DIR)/jre; \
		echo "Extracting JRE..."; \
		# --strip-components=1 is magic: it removes the parent folder in the tarball \
		tar -xzf $(TOOLS_DIR)/jre.tar.gz -C $(TOOLS_DIR)/jre --strip-components=1; \
		rm $(TOOLS_DIR)/jre.tar.gz; \
		echo "==> Hermetic JRE successfully installed to: ./$(TOOLS_DIR)/jre"; \
	else \
		echo "==> Hermetic JRE is already installed."; \
	fi


install-jdtls: install-jre
	@echo "==> Setting up JDTLS sandbox..."
	@mkdir -p $(TOOLS_DIR)
	@if [ ! -x "$(TOOLS_DIR)/jdtls/bin/jdtls" ]; then \
		set -e; \
		echo "Downloading JDTLS 1.57.0..."; \
		# Added -f to curl so it crashes on 404 instead of downloading HTML \
		curl -L -f "https://download.eclipse.org/jdtls/milestones/1.57.0/jdt-language-server-1.57.0-202602261110.tar.gz" -o $(TOOLS_DIR)/jdtls.tar.gz; \
		mkdir -p $(TOOLS_DIR)/jdtls; \
		echo "Extracting JDTLS..."; \
		tar -xzf $(TOOLS_DIR)/jdtls.tar.gz -C $(TOOLS_DIR)/jdtls; \
		rm $(TOOLS_DIR)/jdtls.tar.gz; \
		chmod +x $(TOOLS_DIR)/jdtls/bin/jdtls; \
		echo "==> JDTLS successfully installed to: ./$(TOOLS_DIR)/jdtls"; \
	else \
		echo "==> JDTLS is already installed."; \
	fi


install-mermaid:
	@echo "==> Installing Mermaid CLI sandbox..."
	@if ! command -v npm >/dev/null 2>&1; then \
		echo "ERROR: 'npm' is not installed."; \
		echo "Mermaid CLI requires Node.js. Please install it via your system package manager:"; \
		echo "  macOS: brew install node"; \
		echo "  Linux: sudo apt install nodejs npm"; \
		exit 1; \
	fi
	@mkdir -p $(TOOLS_DIR)
	@if [ ! -x "$(TOOLS_DIR)/node_modules/.bin/mmdc" ]; then \
		echo "Installing @mermaid-js/mermaid-cli locally..."; \
		cd $(TOOLS_DIR) && npm install @mermaid-js/mermaid-cli > /dev/null 2>&1; \
		echo "==> Mermaid CLI successfully installed to: ./$(TOOLS_DIR)/node_modules/.bin/mmdc"; \
	else \
		echo "==> Mermaid CLI is already installed."; \
	fi


clean:
	@echo "==> Cleaning up bin..."
	@rm -rf $(BUILD_DIR)
	@echo "==> Bin removed..."

clean-dep:
	@echo "==> Cleaning up dep..."
	@rm -rf external/
	@echo "==> Dep removed..."

clean-tools:
	@echo "==> Cleaning up tools..."
	@rm -rf $(TOOLS_DIR)
	@echo "==> All local tools removed."

clean-all: clean clean-dep clean-tools


de-git:
	@echo "============================================================"
	@echo " WARNING: Point of No Return."
	@echo " This will PERMANENTLY DESTROY all Git history in this directory."
	@echo " Only run this for localizing distribution."
	@echo "============================================================"
	@read -p "Are you absolutely sure? [y/N] " ans; \
	if [ "$$ans" = "y" ] || [ "$$ans" = "Y" ]; then \
		echo "==> Stripping all Git tracking data..."; \
		find . -name ".git" -exec rm -rf {} +; \
		rm -rf .gitmodules; \
		echo "==> Done."; \
	else \
		echo "==> Aborted de-git operation."; \
	fi

