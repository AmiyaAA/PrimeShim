import java.text.SimpleDateFormat
import java.util.Date
import java.util.Properties
import java.util.TimeZone

plugins { id("com.android.application") }

// ---- build identity -----------------------------------------------------
// versionCode is the UTC build date packed as yyMMddHHmm, so every CI run is a
// strictly increasing upgrade and the version number explains itself.
// versionName mirrors it as 20YY.MM.DD.HHMM.
val stampFormat = SimpleDateFormat("yyMMddHHmm").apply {
    timeZone = TimeZone.getTimeZone("UTC")
}
val buildStamp: String = System.getenv("PRIMESHIM_BUILD_STAMP") ?: stampFormat.format(Date())
// versionCode has to stay inside an Int: minutes since 2026-01-01T00:00:00Z is
// monotonic, time-derived and nowhere near 2^31.
val PROJECT_EPOCH_MINUTES = 1767225600000L / 60000L
val buildVersionCode = ((stampFormat.parse(buildStamp).time / 60000L) - PROJECT_EPOCH_MINUTES).toInt()
val buildVersionName = "20${buildStamp.substring(0, 2)}." +
        "${buildStamp.substring(2, 4)}.${buildStamp.substring(4, 6)}.${buildStamp.substring(6)}"

// ---- signing ------------------------------------------------------------
// The keystore lives in the (private) repository so the signature stays stable
// across CI runs.  Losing it means every installed copy must be reinstalled;
// the certificate is valid until 2056.
val keystorePropertiesFile = rootProject.file("keystore.properties")
val keystoreProperties = Properties().apply {
    if (keystorePropertiesFile.exists()) {
        keystorePropertiesFile.inputStream().use { load(it) }
    }
}

dependencies {
    implementation("androidx.core:core:1.13.1")
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
}

android {
    namespace = "dev.primeshim.android"
    compileSdk = 34
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.primeshim.android"
        minSdk = 26
        targetSdk = 34
        versionCode = buildVersionCode
        versionName = buildVersionName
        buildConfigField("String", "BUILD_STAMP", "\"$buildStamp\"")

        // The guest firmware is 32-bit ARM; 64-bit-only devices cannot run it.
        ndk { abiFilters += listOf("armeabi-v7a") }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
                cFlags += listOf("-O2")
            }
        }
    }

    signingConfigs {
        if (keystorePropertiesFile.exists()) {
            create("stable") {
                storeFile = rootProject.file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        getByName("debug") {
            isDebuggable = true
            if (keystorePropertiesFile.exists()) {
                signingConfig = signingConfigs.getByName("stable")
            }
        }
        getByName("release") {
            isMinifyEnabled = false
            if (keystorePropertiesFile.exists()) {
                signingConfig = signingConfigs.getByName("stable")
            }
        }
    }

    buildFeatures { buildConfig = true }

    externalNativeBuild { cmake { path = file("../CMakeLists.txt") } }

    packaging { jniLibs { useLegacyPackaging = false } }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint { abortOnError = false }
}
