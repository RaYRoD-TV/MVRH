# Diddy Kong Racing VR on Quest

This Android code comes from the Golden Balloon 1.2.0 Quest port. I haven't
built or tested the 1.7.0 update on Quest yet. The Windows checks don't cover it.

It uses SDL2, GLES 3.2 and OpenXR on arm64-v8a. Build the native libraries with
CMake and Android NDK 27, then stage them into the Gradle project.
The existing scripts use a short Windows junction at C:\g\dkrvr and an SDK
at C:\Android\sdk. Set these paths for the local checkout before building.

The immersive flavor is the normal headset app. The panel flavor is for
diagnosis and omits the VR launch category. The app ID is
com.rayrod.goldenballoon; the panel flavor adds .panel.

Use a US 1.1 or European 1.1 ROM in the app's external files directory.
No ROM or game assets are included. Keep signing keys and local SDK settings
out of the repository.

The current Quest APK is available through the Multiverse VR Hub.
