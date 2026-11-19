import { PNG } from "pngjs";
export function decodePpm(buffer) {
  let offset = 0;
  function token() {
    while (offset < buffer.length) {
      if (buffer[offset] === 35) {
        while (offset < buffer.length && buffer[offset] !== 10) offset++;
      } else if (buffer[offset] <= 32) offset++;
      else break;
    }
    const start = offset;
    while (offset < buffer.length && buffer[offset] > 32) offset++;
    return buffer.subarray(start, offset).toString();
  }
  if (token() !== "P6") throw Error("Expected P6 screenshot");
  const width = Number(token()),
    height = Number(token()),
    max = Number(token());
  if (
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    width < 1 ||
    height < 1 ||
    width * height > 16777216 ||
    max !== 255
  )
    throw Error("Unsupported screenshot dimensions");
  if (buffer[offset] === 13 && buffer[offset + 1] === 10) offset += 2;
  else offset++;
  if (buffer.length - offset !== width * height * 3)
    throw Error("Truncated screenshot");
  const image = new PNG({ width, height });
  for (let i = 0, j = offset; i < image.data.length; i += 4, j += 3) {
    image.data[i] = buffer[j];
    image.data[i + 1] = buffer[j + 1];
    image.data[i + 2] = buffer[j + 2];
    image.data[i + 3] = 255;
  }
  return image;
}
export function encode(image) {
  return PNG.sync.write(image, { colorType: 2 });
}
export function resize(image, maxWidth = 640) {
  if (image.width <= maxWidth) return image;
  const width = maxWidth,
    height = Math.max(1, Math.round((image.height * width) / image.width)),
    out = new PNG({ width, height });
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const a =
          (Math.floor((y * image.height) / height) * image.width +
            Math.floor((x * image.width) / width)) *
          4,
        b = (y * width + x) * 4;
      image.data.copy(out.data, b, a, a + 4);
    }
  return out;
}
export function combine(images) {
  if (!images.length) throw Error("No screenshots");
  const gap = 8,
    width = images.reduce((s, i) => s + i.width, 0) + gap * (images.length - 1),
    height = Math.max(...images.map((i) => i.height));
  if (width * height > 33554432) throw Error("Combined capture too large");
  const out = new PNG({ width, height });
  out.data.fill(24);
  for (let n = 3; n < out.data.length; n += 4) out.data[n] = 255;
  let x = 0;
  for (const image of images) {
    PNG.bitblt(image, out, 0, 0, image.width, image.height, x, 0);
    x += image.width + gap;
  }
  return out;
}
