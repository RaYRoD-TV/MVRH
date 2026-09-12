# Diddy Kong Racing VR on Quest

This Android source is carried forward from the Golden Balloon 1.2.0 Quest
port. The 1.7.0 source update has been built and checked on Windows. Its
Android build and headset behavior have not yet been qualified.

The game uses SDL2, GLES 3.2 and OpenXR on arm64-v8a. Native libraries are
built with CMake and Android NDK 27, then staged into the Gradle project.
The existing scripts use a short Windows junction at C:\g\dkrvr and an SDK
at C:\Android\sdk. Set these paths for the local checkout before building.

The immersive flavor is the standalone headset application. The panel
flavor is a separate diagnostic application without the VR launch category.
The application ID is com.rayrod.goldenballoon; the panel flavor adds .panel.

The ROM belongs in the application's external files directory. Supported
cartridge revisions are US 1.1 and European 1.1. No ROM or game assets are
included. Release signing keys and local SDK settings must remain private.

The existing Quest download remains available through the Multiverse VR Hub.
This source patch does not publish a replacement APK.
