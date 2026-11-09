TITLEID ?= 4200000000505341
MODULE  := gwm-stats-switch
VERSION := $(strip $(shell cat VERSION))

# $(MODULE) is also a directory name, so without .PHONY make considers the
# target up to date and never descends into it.
.PHONY: all clean dist check-version $(MODULE)

all: dist

$(MODULE):
	@$(MAKE) --no-print-directory -C $(MODULE)

# Lays out exactly what gets copied to the SD card, so `dist/` can be dragged
# onto the card root as-is.
check-version:
	@grep -Eq '"version"[[:space:]]*:[[:space:]]*"$(VERSION)"' $(MODULE)/toolbox.json || \
		(echo "toolbox.json version does not match VERSION=$(VERSION)" >&2; exit 1)

dist: check-version $(MODULE)
	@rm -rf dist
	@mkdir -p dist/atmosphere/contents/$(TITLEID)/flags
	@touch dist/atmosphere/contents/$(TITLEID)/flags/boot2.flag
	@cp $(MODULE)/$(MODULE).nsp dist/atmosphere/contents/$(TITLEID)/exefs.nsp
	@cp $(MODULE)/toolbox.json  dist/atmosphere/contents/$(TITLEID)/toolbox.json
	@sed 's/@VERSION@/$(VERSION)/g' README.dist.txt > dist/README.txt
	@echo "Built dist/atmosphere/contents/$(TITLEID)/ (version $(VERSION))"

clean:
	@$(MAKE) --no-print-directory -C $(MODULE) clean
	@rm -rf dist
