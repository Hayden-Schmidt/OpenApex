plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.openapex.androidauto"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.openapex.androidauto"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    // RelayRecorder (dev-only drive capture) gates itself on BuildConfig.DEBUG, so BuildConfig has
    // to be generated. AGP 8 defaults this off.
    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // The wire formats: packet, call, settings TLV and rider mesh. Shared with iOS verbatim --
    // one encoder per format, so the two platforms cannot drift a byte apart.
    implementation(project(":shared"))
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("com.google.android.gms:play-services-location:21.3.0")
    // Rider mesh keys at rest (RiderStore).
    implementation("androidx.security:security-crypto:1.1.0")

    testImplementation("junit:junit:4.13.2")
}
