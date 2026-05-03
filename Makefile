BUILD_DIR := build

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


clean:
	@echo "==> Cleaning up bin..."
	rm -rf $(BUILD_DIR)


clean-all:
	@echo "==> Cleaning up bin and dep..."
	rm -rf $(BUILD_DIR)
	rm -rf external/


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

# install-mermaid:
# 	@mkdir -p $(TOOLS_DIR)
# 	@if ! command -v mmdc >/dev/null 2>&1; then \
# 		echo "Installing Mermaid CLI (requires npm)..."; \
# 		npm install -g @mermaid-js/mermaid-cli; \
# 	else \
# 		echo "Mermaid CLI is already installed."; \
# 	fi
#
#
# install-all-langjava:
# 	@echo "==> Setting up Java Environment..."
# 		@if [ ! -d "$(TOOLS_DIR)/jdtls" ]; then \
# 			echo "Downloading JDTLS..."; \
# 			curl -L "https://download.eclipse.org/jdtls/milestones/1.57.0/jdt-language-server-1.57.0-202402151200.tar.gz" -o $(TOOLS_DIR)/jdtls.tar.gz; \
# 			mkdir -p $(TOOLS_DIR)/jdtls; \
# 			tar -xzf $(TOOLS_DIR)/jdtls.tar.gz -C $(TOOLS_DIR)/jdtls; \
# 			rm $(TOOLS_DIR)/jdtls.tar.gz; \
# 			echo "JDTLS installed locally to $(TOOLS_DIR)/jdtls"; \
# 		else \
# 			echo "JDTLS is already installed."; \
# 		fi
#
# install-all-langcpp:
#
#
#
# install-all-langpython:



