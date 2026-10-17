::lsfg-vk-android-compat::

A performance- and portability-focused Android fork of lsfg-vk, developed for integration with GameNative and native Vulkan frame generation on Android.

This fork extends the original Android port beyond its initial Turnip/Adreno-oriented assumptions, with the goal of making LSFG operate correctly on any Android Vulkan implementation exposing the required Vulkan and AHardwareBuffer capabilities.

Key improvements

- Cross-vendor Android Vulkan support
  
  - Refactored Android interoperability around Vulkan capabilities rather than GPU/vendor-specific behavior.
  - Validated functional frame generation on Samsung Xclipse 940 using the stock Vulkan driver.
  - Retains compatibility with the existing Adreno/Turnip path.

- Native AHardwareBuffer interoperability
  
  - Uses "VK_ANDROID_external_memory_android_hardware_buffer" for Android image sharing.
  - Imports caller-provided AHardwareBuffers directly into LSFG's Vulkan device.
  - Handles dedicated allocations, queue-family ownership transitions, and Android-specific synchronization requirements.

- Correct Vulkan WSI interception
  
  - Hooks the real application's "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", and "vkQueuePresentKHR" paths.
  - Preserves Android Vulkan loader/proc-address behavior while allowing LSFG to operate directly in the presentation chain.

- Stable generated-frame presentation
  
  - Corrected binary semaphore chaining between application frames, generated frames, and "vkQueuePresentKHR".
  - Preserves the validated cross-device AHB synchronization path.
  - Tested at sustained 2× frame-generation output on Xclipse hardware.

- Clean LSFG disable/pass-through behavior
  
  - Disabling LSFG no longer requires destroying or unnecessarily altering the application's swapchain.
  - Off-state presentation follows the normal application path to minimize stutter and residual frame-generation overhead.

- Present-path performance optimizations
  
  - Reuses AHB handoff fences and present semaphore storage.
  - Reduces per-frame allocations and CPU overhead.
  - Uses transient command pools where appropriate.
  - Fixes temporary wrapper ownership/leak paths.
  - Debounces configuration polling instead of performing filesystem work every frame.

- Runtime diagnostics and telemetry
  
  - WSI pointer/provenance tracing for Vulkan loader debugging.
  - Process and module lifecycle diagnostics.
  - Sustained LSFG input/output statistics for verifying that generated frames are actually reaching presentation.
  - Runtime contracts designed to catch regressions in Android synchronization and presentation behavior.

- Regression-tested Android contracts
  
  - Present semaphore chaining.
  - Off-state passthrough.
  - WSI interception/provenance.
  - Runtime statistics.
  - AHB portability and synchronization behavior.

Project goal

The long-term objective is to make LSFG on Android capability-driven rather than Adreno-driven: if an Android GPU and Vulkan driver expose the Vulkan, synchronization, shader, and AHardwareBuffer functionality LSFG requires, the implementation should be able to use it without relying on undocumented Turnip-specific behavior.

The fork is currently developed alongside Ragnarok93/GameNative, where it provides the native LSFG Vulkan layer and Android frame-generation runtime.
