# Third-party notice

This example adapts the cube mesh, UV coordinates, camera/presentation
parameters, and shader lighting from the Vulkan-Tools vkcube sample at tag
`vulkan-sdk-1.4.357.0`. `lunarg_logo_png.hpp` embeds that sample's texture from
`cube/lunarg.ppm.h`:

https://github.com/KhronosGroup/Vulkan-Tools/blob/vulkan-sdk-1.4.357.0/cube/lunarg.ppm.h

The texture bytes were losslessly converted from the upstream PPM
representation to a PNG and then Base64-encoded for compact, source-only
embedding. The decoded image content is unchanged. The C++ and shader code was
rewritten for the clean_gfx API and Slang.

The upstream Vulkan cube sample carries these notices:

Copyright (c) 2015-2019 The Khronos Group Inc.
Copyright (c) 2015-2019 Valve Corporation
Copyright (c) 2015-2019 LunarG, Inc.

The asset is redistributed under the Apache License, Version 2.0. A complete
copy is included beside this notice in `LICENSE-Apache-2.0.txt`.
