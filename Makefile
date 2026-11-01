# Makefile for the standalone openmediavault-filebrowser-quantum plugin.
#
# This plugin lives in its OWN repository (it is not built inside the
# openmediavault monorepo), so it cannot "include ../openmediavault.mk"
# (that path only resolves in deb/). The targets below reproduce the
# relevant parts of the official deb/openmediavault.mk:
#   * omv_build_pot - extract _("...") messages from the Workbench YAML
#                     manifests and the PHP/INC sources into the .pot
#                     template with xgettext --keyword=_.
#   * workbench / i18n - run omv-mkworkbench, the same step the package
#                     postinst triggers through "dpkg-trigger update-workbench".
#
# Normal Debian package builds do NOT need this file: debian/rules only
# uses debhelper. Use these targets on a development machine to refresh
# the translation template, validate manifests, run test suites, and rebuild
# Workbench assets.

OMV_PACKAGE    := openmediavault-filebrowser-quantum
OMV_VERSION    := 8.1.2
OMV_PROJECT_DIR := $(CURDIR)
OMV_WORKBENCH_DIR := $(OMV_PROJECT_DIR)/usr/share/openmediavault/workbench
OMV_LOCALE_DIR   := $(OMV_PROJECT_DIR)/usr/share/openmediavault/locale
OMV_POT          := $(OMV_LOCALE_DIR)/$(OMV_PACKAGE).pot

# Same flags deb/openmediavault.mk uses. YAML is parsed with the Python
# language scanner (the _("...") calls are Python-compatible strings).
OMV_XGETTEXT_ARGS := --force-po --no-location \
	--package-name=$(OMV_PACKAGE) --package-version=$(OMV_VERSION) \
	--keyword=_ --language=Python --from-code=UTF-8

.PHONY: help pot omv_build_pot po-update locale test validate i18n workbench clean

help:
	@echo "Targets:"
	@echo "  make validate             Parse every YAML and check i18n consistency (python3)"
	@echo "  make test                 Run anti-recursion and declarative config test suite"
	@echo "  make locale               Rebuild .pot, zh_CN and zh_TW .po via scripts/build_locale.py"
	@echo "  make pot / omv_build_pot  Regenerate $(OMV_PACKAGE).pot from YAML/PHP sources (gettext)"
	@echo "  make po-update            Merge new msgids into zh_CN/zh_TW .po (msgmerge)"
	@echo "  make i18n                 Rebuild assets/i18n/*.json via omv-mkworkbench (OMV host)"
	@echo "  make workbench            Rebuild all Workbench assets via omv-mkworkbench (OMV host)"

# Extract translatable strings, overwriting the .pot template.
pot: omv_build_pot
omv_build_pot:
	@command -v xgettext >/dev/null 2>&1 || { \
		echo "ERROR: gettext (xgettext) is required. apt-get install gettext" >&2; exit 1; }
	@mkdir -p "$(OMV_LOCALE_DIR)"
	@find "$(OMV_WORKBENCH_DIR)" -type f -iname '*.yaml' -print0 \
		| xargs -0r xgettext $(OMV_XGETTEXT_ARGS) -o "$(OMV_POT)" -
	@find "$(OMV_PROJECT_DIR)/usr/share/openmediavault" \
		-type f \( -iname '*.inc' -o -iname '*.php' \) -print0 \
		| xargs -0r xgettext $(OMV_XGETTEXT_ARGS) -j -o "$(OMV_POT)" -
	@# Drop c-format flags (same post-step as openmediavault.mk).
	@sed -i '/^#, c-format/d' "$(OMV_POT)"
	@echo "Updated $(OMV_POT)"

# Merge freshly extracted msgids into the maintained translations.
po-update: omv_build_pot
	@command -v msgmerge >/dev/null 2>&1 || { \
		echo "ERROR: gettext (msgmerge) is required. apt-get install gettext" >&2; exit 1; }
	@for lang in zh_CN zh_TW; do \
		po="$(OMV_LOCALE_DIR)/$$lang/$(OMV_PACKAGE).po"; \
		if [ -f "$$po" ]; then \
			msgmerge --update --no-fuzzy-matching --backup=none "$$po" "$(OMV_POT)"; \
			echo "Merged $$po"; \
		else \
			echo "ERROR: $$po not found"; exit 1; \
		fi; \
	done

# Automated builder for .pot and 100% translated Chinese catalogs without host gettext tools
locale:
	@python3 "$(OMV_PROJECT_DIR)/scripts/build_locale.py"

# Unit tests
test:
	@python3 "$(OMV_PROJECT_DIR)/scripts/test_anti_recursion.py"
	@if command -v php >/dev/null 2>&1; then php "$(OMV_PROJECT_DIR)/scripts/test_source_path.php"; else echo "php not found; skipping scripts/test_source_path.php"; fi

# Host-independent checks: every manifest parses, component names are
# unique, routes reference existing components, and every _() msgid in
# the YAML exists in the .pot and the two .po files.
validate:
	@python3 "$(OMV_PROJECT_DIR)/scripts/validate-workbench.py"

# Rebuild the Workbench i18n JSON (locale/**/*.po -> assets/i18n/<lang>.json).
# Only meaningful on a machine with openmediavault installed; the package
# postinst runs this automatically through the update-workbench trigger.
i18n:
	@command -v omv-mkworkbench >/dev/null 2>&1 || { \
		echo "ERROR: omv-mkworkbench not found; run this on an OMV host" >&2; exit 1; }
	@omv-mkworkbench --verbose i18n

# Full Workbench rebuild (route/navigation/component + i18n).
workbench:
	@command -v omv-mkworkbench >/dev/null 2>&1 || { \
		echo "ERROR: omv-mkworkbench not found; run this on an OMV host" >&2; exit 1; }
	@omv-mkworkbench --verbose all

clean:
	@rm -f "$(OMV_PROJECT_DIR)/messages.po"
