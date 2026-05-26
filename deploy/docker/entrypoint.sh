#!/bin/bash
set -euo pipefail

BUILD_TYPE="${1:-${BUILD_TYPE:-Release}}"
ANDROID_ABIS="${ANDROID_ABIS:-arm64-v8a}"  # Options: arm64-v8a, armeabi-v7a, or both with semicolon

case "${BUILD_TYPE}" in
    Release|Debug|RelWithDebInfo|MinSizeRel) ;;
    *)
        echo "Error: Invalid BUILD_TYPE: ${BUILD_TYPE}"
        echo "Usage: $0 [Release|Debug|RelWithDebInfo|MinSizeRel]"
        exit 1
        ;;
esac

if [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
    # Validate required Android environment variables
    for var in QT_HOST_PATH QT_ROOT_DIR_ARM64 ANDROID_SDK_ROOT; do
        if [[ -z "${!var:-}" ]]; then
            echo "Error: Required environment variable $var is not set" >&2
            exit 1
        fi
    done

    if [[ -z "${QT_ANDROID_KEYSTORE_PATH:-}" || -z "${QT_ANDROID_KEYSTORE_ALIAS:-}" || -z "${QT_ANDROID_KEYSTORE_STORE_PASS:-}" || -z "${QT_ANDROID_KEYSTORE_KEY_PASS:-}" ]]; then
        export QT_ANDROID_KEYSTORE_PATH=/tmp/debug.keystore
        export QT_ANDROID_KEYSTORE_ALIAS=androiddebugkey
        export QT_ANDROID_KEYSTORE_STORE_PASS=android
        export QT_ANDROID_KEYSTORE_KEY_PASS=android

        if [[ ! -f "${QT_ANDROID_KEYSTORE_PATH}" ]]; then
            keytool -genkey -v \
                -keystore "${QT_ANDROID_KEYSTORE_PATH}" \
                -storepass "${QT_ANDROID_KEYSTORE_STORE_PASS}" \
                -alias "${QT_ANDROID_KEYSTORE_ALIAS}" \
                -keypass "${QT_ANDROID_KEYSTORE_KEY_PASS}" \
                -keyalg RSA \
                -keysize 2048 \
                -validity 10000 \
                -dname "CN=Android Debug,O=Android,C=US"
        fi
    fi

    # Qt 6.10's Android QML packaging scans module inputs during configure.
    # Pre-generate build-tree QML artifacts so scanner inputs already exist.
    mkdir -p \
        /project/build/src/UI/AppSettings/generated \
        /project/build/src/AutoPilotPlugins/PX4/generated \
        /project/build/src/AutoPilotPlugins/APM/generated
    (
        cd /project/source
        PYTHONPATH=/project/source python3 -m tools.generators.settings_qml.generate_pages \
        --output-dir /project/build/src/UI/AppSettings/generated
    )
    (
        cd /project/source
        PYTHONPATH=/project/source python3 -m tools.generators.config_qml.generate_pages \
        --pages-dir /project/source/src/AutoPilotPlugins/PX4/VehicleConfig \
        --output-dir /project/build/src/AutoPilotPlugins/PX4/generated
    )
    (
        cd /project/source
        PYTHONPATH=/project/source python3 -m tools.generators.config_qml.generate_pages \
        --pages-dir /project/source/src/AutoPilotPlugins/APM/VehicleConfig \
        --output-dir /project/build/src/AutoPilotPlugins/APM/generated
    )

    echo "Building QGroundControl for Android (${BUILD_TYPE})..."
    "${QT_ROOT_DIR_ARM64}/bin/qt-cmake" -S /project/source -B /project/build -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DQT_HOST_PATH="${QT_HOST_PATH}" \
        -DQT_ANDROID_ABIS="${ANDROID_ABIS}" \
        -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT}" \
        -DGStreamer_ROOT_DIR="${GStreamer_ROOT_DIR:-}" \
        -DQT_ANDROID_SIGN_APK=ON
    cmake --build /project/build --target all --config "${BUILD_TYPE}" --parallel
else
    echo "Building QGroundControl (${BUILD_TYPE})..."
    qt-cmake -S /project/source -B /project/build -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build /project/build --target all --parallel
    cmake --install /project/build --config "${BUILD_TYPE}"
fi

echo "Build complete!"
