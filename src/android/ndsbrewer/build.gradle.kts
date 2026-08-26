// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// bannertool/makerom (vendored under externals/ds_forwarder_tools/android-arm64-v8a/,
// MIT licensed -- see LICENSE-*.txt alongside them) are packaged as
// jniLibs/arm64-v8a/libndsbrewer_{bannertool,makerom}.so -- they are real
// ELF executables, not shared libraries, but naming them as .so and
// placing them under jniLibs is what makes Android's PackageManager
// extract them to nativeLibraryDir at install time, which -- unlike
// filesDir/cacheDir -- is guaranteed to stay executable. NDSBrewer shells
// out to them via ProcessBuilder at runtime (see ForwarderBuilder.kt).
android {
    namespace = "org.citra.citra_emu.ndsbrewer"

    compileSdkVersion = "android-35"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = true
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }

    defaultConfig {
        applicationId = "org.azahar_emu.ndsbrewer"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            //noinspection ChromeOsAbiSupport
            abiFilters += listOf("arm64-v8a")
        }
    }

    // Must be signed with the exact same key as :app's own release
    // build: InstallCiaBridgeActivity there is guarded by a
    // signature-level permission (see AndroidManifest.xml there), so a
    // differently-signed NDSBrewer release couldn't ever use the
    // install hand-off against a real release install of the main app.
    // Same ANDROID_KEYSTORE_FILE/ANDROID_KEY_ALIAS/ANDROID_KEYSTORE_PASS
    // env vars :app's own signingConfigs.release reads, set up the same
    // way by .ci/android.sh -- falls back to debug signing when they
    // aren't present (a from-source/fork build without the real
    // release secrets), matching :app's own fallback exactly.
    val keystoreFile = System.getenv("ANDROID_KEYSTORE_FILE")
    signingConfigs {
        // Same checked-in debug key :app uses (src/android/ci-debug.keystore)
        // -- see the matching comment there. Without this override, this
        // module's debug-signing fallback used AGP's implicit, per-machine
        // ~/.android/debug.keystore, which is why a real device ended up
        // with :app and :ndsbrewer signed by two different random debug
        // keys depending on which CI run built which artifact.
        getByName("debug") {
            storeFile = rootProject.file("ci-debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        if (keystoreFile != null) {
            create("release") {
                storeFile = file(keystoreFile)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASS")
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("ANDROID_KEYSTORE_PASS")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystoreFile != null) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
        debug {
            applicationIdSuffix = ".debug"
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-ktx:1.9.2")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.9.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.constraintlayout:constraintlayout:2.2.0")
}
