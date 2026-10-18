plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "io.microwallet.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.microwallet.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 6
        versionName = "0.7.1"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Личная сборка: подписываем отладочным ключом, чтобы APK сразу ставился
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        resources.excludes += setOf("META-INF/versions/9/OSGI-INF/MANIFEST.MF", "META-INF/DEPENDENCIES")
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
        unitTests.all {
            // Симулятор устройства (tools/sim/build.sh): MW_SIM=/путь/к/mwsim ./gradlew test
            it.systemProperty("mw.sim", System.getenv("MW_SIM") ?: "")
            it.systemProperty("mw.words", rootProject.file("../host/bip39_english.txt").absolutePath)
            // Монеты — приложения WebAssembly (sh apps/build.sh)
            it.systemProperty("mw.apps", rootProject.file("../apps/dist").absolutePath)
        }
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    // X25519, ChaCha20-Poly1305, Keccak — канал BLE и адреса BSC
    implementation("org.bouncycastle:bcprov-jdk18on:1.79")
    // QR: показать адрес и отсканировать адрес получателя
    implementation("com.google.zxing:core:3.5.3")
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
