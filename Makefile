BUILD_DIR := build

-include .env

# test 
T ?= .*

.PHONY: all config build clean 

all: config build


config:
	cmake -S . -B $(BUILD_DIR) 


build: config
	cmake --build $(BUILD_DIR) -j -- --no-print-directory


clean:
	rm -rf $(BUILD_DIR)


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



update-vendor: 
	git submodule update --remote
