idf_component_register(
    SRCS
        "bme68x.c"
        "bme68x_esp32_i2c.c"
    INCLUDE_DIRS
        "."
    REQUIRES driver
)

# Use Bosch floating-point output path (temperature °C, humidity %RH, etc.)
# This makes printing/logging much simpler.
target_compile_definitions(${COMPONENT_LIB} PRIVATE BME68X_USE_FPU=1)