plugins {
  alias(libs.plugins.android.application)
  alias(libs.plugins.kotlin.android)
  alias(libs.plugins.kotlin.compose)
}

android {
  namespace = "com.einx.send"
  compileSdk = 35

  defaultConfig {
    applicationId = "com.einx.send"
    minSdk = 26
    targetSdk = 35
    versionCode = 1
    // Tracks the firmware release this app ships alongside; the two halves of the
    // transfer protocol are versioned together on purpose.
    versionName = "1.6.17"

    testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
  }

  buildTypes {
    release {
      // The app is one screen and a BLE client; shrinking buys nothing and would only
      // make a crash report from a user's phone harder to read.
      isMinifyEnabled = false
      proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")

      // Signed with the debug key so the APK attached to the GitHub release installs
      // as-is. Anyone shipping this through a store should replace this block.
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
}

dependencies {
  implementation(libs.androidx.core.ktx)
  implementation(libs.androidx.lifecycle.runtime.ktx)
  implementation(libs.androidx.lifecycle.viewmodel.compose)
  implementation(libs.androidx.lifecycle.runtime.compose)
  implementation(libs.androidx.activity.compose)
  implementation(libs.androidx.documentfile)

  implementation(platform(libs.androidx.compose.bom))
  implementation(libs.androidx.compose.ui)
  implementation(libs.androidx.compose.ui.graphics)
  implementation(libs.androidx.compose.ui.tooling.preview)
  implementation(libs.androidx.compose.material3)

  testImplementation(libs.junit)
  // android.jar's org.json is a stub that throws on every call, so the JVM unit tests need a
  // real implementation to exercise the message encoding against.
  testImplementation(libs.json)
  testImplementation(libs.kotlinx.coroutines.test)
}
