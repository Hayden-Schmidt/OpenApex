plugins {
    id("org.jetbrains.kotlin.multiplatform")
    id("com.android.library")
}

kotlin {
    androidTarget {
        compilations.all {
            compileTaskProvider.configure {
                compilerOptions.jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
            }
        }
    }

    // The JVM target exists so commonMain and commonTest can be built and run on Windows, where
    // there is no Mac and so no Kotlin/Native Apple target. `:shared:jvmTest` is the real gate on
    // the shared half; it is not shipped anywhere.
    jvm()

    // iosX64/iosArm64/iosSimulatorArm64 land in Phase C, with the Mac. Declaring them here now
    // would only make Gradle configuration fail on this machine. Their absence costs nothing in
    // the meantime: commonMain compiles against the COMMON stdlib whatever the target list is, so
    // a stray java.* import fails here and now rather than surviving until the Mac arrives.

    sourceSets {
        commonTest.dependencies {
            implementation(kotlin("test"))
        }
    }
}

android {
    namespace = "org.openapex.shared"
    compileSdk = 36
    defaultConfig {
        minSdk = 26
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
