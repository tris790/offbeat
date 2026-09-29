## What we need to build
- simple performant opengl rendered opengl 4.6 (with abstracted interface for rendering if we change in future) should be self contained
- utf8 simple performant strings in C ()
- a simple and performant way to draw fonts on screen sdf
- a simple and performant way to draw things like rectangle, triangle and circle
- a simple and performant way to draw images
- a simple and performant memory allocator supports virtual memory allocation
- a simple and performant way to handle user input (keyboard, clicking, mouse movement, scroll wheel)

## project structure
each function in src/layer_folder has prefix for the folder like core_ platform_ game_ 
- src
    - core -> this is code that can be reused on other projects (string, allocators, renderer, etc)
    - game -> this is the code specific for this application that wont be reused and works accross all platforms
    - platform -> ALL OS specific code goes here linux wayland, windows (unimplemented)
    - third_party -> stb single header style
    - main.c -> very simple file that does nothing but call the different layers to create a window and draw each shape.
- build.sh simple bash script that compiles the application must be sub 1 sec even as codebase size increase to 100k lines. I dont want to have to modify this file when I add code.
The calling is ./build.sh or build.sh release