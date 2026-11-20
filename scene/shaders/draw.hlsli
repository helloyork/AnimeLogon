// Every node is one quad: a unit square carried onto the canvas by an affine transform. The pixel
// shader takes a texture over the quad, a colour, a gradient, or an offscreen layer pixel for pixel,
// and a mask over the node's box. Colours leave it premultiplied; the blend state does the rest.
cbuffer Draw : register(b0) {
    float4 toCanvas0, toCanvas1;      // canvas pixel = (dot(toCanvas0.xyz, (u, v, 1)), dot(toCanvas1.xyz, ...))
    float4 fromCanvas0, fromCanvas1;  // the box's unit square from a canvas pixel, for a whole-canvas quad
    float4 canvas;                    // xy: the target's size in pixels
    float4 sourceRect, maskRect;      // xy: the used part of the texture, as a fraction of it
    float4 maskMap;                   // the box's unit square from the quad's: xy scale, zw offset
    float4 color;
    float4 stops[8];
    float4 stopAt0, stopAt1;
    float opacity, mode, useMask, stopCount;
    float gradient, premultiplied, whole, unused;
};
