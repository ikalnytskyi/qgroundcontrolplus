# ============================================================================
# Custom Build Configuration Overrides
# Template for customizing QGroundControl branding and feature set
# ============================================================================

# ----------------------------------------------------------------------------
# Application Branding
# ----------------------------------------------------------------------------
set(QGC_APP_NAME "Custom-QGroundControl" CACHE STRING "App Name" FORCE)

# ----------------------------------------------------------------------------
# Feature Set Customization
# ----------------------------------------------------------------------------

# Match regular daily feature coverage for ArduPilot support
set(QGC_DISABLE_APM_MAVLINK OFF CACHE BOOL "Disable APM Dialect" FORCE)
set(QGC_DISABLE_APM_PLUGIN OFF CACHE BOOL "Disable APM Plugin" FORCE)
set(QGC_DISABLE_APM_PLUGIN_FACTORY OFF CACHE BOOL "Disable APM Plugin Factory" FORCE)

# This custom build targets ArduPilot vehicles only, but still keeps PX4
# support code compiled for shared utilities such as log analysis metadata.
set(QGC_DISABLE_PX4_PLUGIN OFF CACHE BOOL "Disable PX4 Plugin" FORCE)
set(QGC_DISABLE_PX4_PLUGIN_FACTORY ON CACHE BOOL "Disable PX4 Plugin Factory" FORCE)

# Keep the standard daily media/device defaults explicitly enabled
set(QGC_NO_SERIAL_LINK OFF CACHE BOOL "Disable serial links" FORCE)
set(QGC_ENABLE_UVC ON CACHE BOOL "Enable UVC device support" FORCE)
set(QGC_ENABLE_GST_VIDEOSTREAMING ON CACHE BOOL "Enable GStreamer video backend" FORCE)
set(QGC_ENABLE_QT_VIDEOSTREAMING OFF CACHE BOOL "Enable QtMultimedia video backend" FORCE)

# Private Android tablet deployment relies on importing staged config from shared storage.
# This is intentionally not Play Store compliant, but is acceptable for this controlled build.
set(QGC_ANDROID_ENABLE_MANAGE_EXTERNAL_STORAGE ON CACHE BOOL "Request MANAGE_EXTERNAL_STORAGE" FORCE)
