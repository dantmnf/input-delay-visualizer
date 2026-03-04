# Input Delay Visualizer

This AI slop may help you visualize and understand input delay in games. 

## Usage

### Click-to-Screen Delay Visualization

The crosshair cursor changes color on left mouse button down. With an LED hooked up to the hardware mouse button, you can measure the delay from the click to the crosshair change.

### Swapchain vs Hardware Cursor

The crosshair is rendered in the swapchain, so it will be delayed by the time it takes for a frame to be rendered and presented. The OS cursor is usually rendered on a dedicated plane without vsync.

With that in mind, you can know the delay of swapchain and OS composition by the time the crosshair delays after OS cursor. This tool also provides estimation of OS cursor position, given an adjustable number of delayed frames. (Best with custom mappings or macros that move the mouse at a constant speed.)

### Choosing a Renderer (Direct3D, OpenGL, Vulkan, etc)

Use `SDL_RENDER_DRIVER` environment variable to choose a renderer. Also `SDL_VIDEODRIVER` to check difference between `x11` and `wayland` on Linux.

Set `SDL_RENDER_DRIVER=help` to list available renderers.
