import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import type { GLTF, GLTFParser } from 'three/addons/loaders/GLTFLoader.js';

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function object(value: unknown): Record<string, unknown> {
  if (!isObject(value)) throw new Error('Invalid asset metadata');
  return value;
}
function index(value: unknown): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) throw new Error('Invalid asset index');
  return value;
}
function array(value: unknown): unknown[] {
  if (!Array.isArray(value)) throw new Error('Invalid asset array');
  return value;
}

const formats = new Map<number, THREE.CompressedPixelFormat>([
  [71, THREE.RGBA_S3TC_DXT1_Format], [72, THREE.RGBA_S3TC_DXT1_Format],
  [74, THREE.RGBA_S3TC_DXT3_Format], [75, THREE.RGBA_S3TC_DXT3_Format],
  [77, THREE.RGBA_S3TC_DXT5_Format], [78, THREE.RGBA_S3TC_DXT5_Format],
  [80, THREE.RED_RGTC1_Format], [81, THREE.SIGNED_RED_RGTC1_Format],
  [83, THREE.RED_GREEN_RGTC2_Format], [84, THREE.SIGNED_RED_GREEN_RGTC2_Format],
  [95, THREE.RGB_BPTC_UNSIGNED_Format], [96, THREE.RGB_BPTC_SIGNED_Format],
  [98, THREE.RGBA_BPTC_Format], [99, THREE.RGBA_BPTC_Format],
]);

function bc1Preview(data: Uint8Array, width: number, height: number): THREE.DataTexture {
  if (width > 4096 || height > 4096) throw new Error('Portable BC1 preview is limited to 4096x4096');
  const output = new Uint8Array(width * height * 4);
  const source = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const color = (value: number) => [
    Math.round(((value >> 11) & 31) * 255 / 31),
    Math.round(((value >> 5) & 63) * 255 / 63),
    Math.round((value & 31) * 255 / 31), 255,
  ];
  let offset = 0;
  for (let by = 0; by < height; by += 4) for (let bx = 0; bx < width; bx += 4) {
    const first = source.getUint16(offset, true), second = source.getUint16(offset + 2, true);
    const a = color(first), b = color(second);
    const palette = [a, b, [0, 0, 0, 255], [0, 0, 0, first > second ? 255 : 0]];
    for (let channel = 0; channel < 3; channel++) {
      const av = a[channel], bv = b[channel], c = palette[2], d = palette[3];
      if (av === undefined || bv === undefined || !c || !d) throw new Error('Invalid BC1 palette');
      c[channel] = first > second ? Math.floor((2 * av + bv) / 3) : Math.floor((av + bv) / 2);
      d[channel] = first > second ? Math.floor((av + 2 * bv) / 3) : 0;
    }
    const selectors = source.getUint32(offset + 4, true);
    for (let y = 0; y < 4 && by + y < height; y++) for (let x = 0; x < 4 && bx + x < width; x++) {
      const pixel = palette[(selectors >>> (2 * (y * 4 + x))) & 3];
      if (!pixel) throw new Error('Invalid BC1 selector');
      output.set(pixel, ((by + y) * width + bx + x) * 4);
    }
    offset += 8;
  }
  const texture = new THREE.DataTexture(output, width, height, THREE.RGBAFormat);
  texture.magFilter = THREE.LinearFilter;
  texture.minFilter = THREE.LinearFilter;
  texture.flipY = false;
  texture.needsUpdate = true;
  return texture;
}

/** GLBs retain the original blocks; BC1 can also be previewed on GPUs without S3TC. */
export function ddsTexture(buffer: ArrayBuffer, supports?: (format: THREE.CompressedPixelFormat) => boolean): THREE.CompressedTexture | THREE.DataTexture {
  const view = new DataView(buffer);
  if (buffer.byteLength < 128 || view.getUint32(0, true) !== 0x20534444 ||
      view.getUint32(4, true) !== 124 || view.getUint32(76, true) !== 32 ||
      (view.getUint32(80, true) & 4) === 0) throw new Error('Invalid DDS header');
  const width = view.getUint32(16, true), height = view.getUint32(12, true);
  const count = Math.max(view.getUint32(28, true), 1), fourCC = view.getUint32(84, true);
  if (!width || !height || width > 16384 || height > 16384 || count > 1 + Math.floor(Math.log2(Math.max(width, height))) ||
      view.getUint32(24, true) > 1 || (view.getUint32(112, true) & 0x20fe00) !== 0) throw new Error('Unsupported DDS dimensions, mip count or shape');
  let offset = 128;
  let dxgi: number;
  if (fourCC === 0x30315844) {
    if (buffer.byteLength < 148 || view.getUint32(132, true) !== 3 ||
        (view.getUint32(136, true) & 4) !== 0 || view.getUint32(140, true) !== 1) throw new Error('DDS DX10 requires one 2D texture');
    offset = 148;
    dxgi = view.getUint32(128, true);
  } else {
    const legacy = new Map([[0x31545844, 71], [0x33545844, 74], [0x35545844, 77],
      [0x31495441, 80], [0x55344342, 80], [0x53344342, 81],
      [0x32495441, 83], [0x55354342, 83], [0x53354342, 84]]);
    const found = legacy.get(fourCC);
    if (found === undefined) throw new Error('Unknown DDS FourCC');
    dxgi = found;
  }
  const format = formats.get(dxgi);
  if (format === undefined) throw new Error('Unknown or typeless DDS format');
  const blockBytes = [71, 72, 80, 81].includes(dxgi) ? 8 : 16;
  const mipmaps: { data: Uint8Array; width: number; height: number }[] = [];
  let mipWidth = width, mipHeight = height;
  for (let level = 0; level < count; level++) {
    const length = Math.ceil(mipWidth / 4) * Math.ceil(mipHeight / 4) * blockBytes;
    if (offset + length > buffer.byteLength) throw new Error('Truncated DDS mip chain');
    mipmaps.push({ data: new Uint8Array(buffer, offset, length), width: mipWidth, height: mipHeight });
    offset += length;
    mipWidth = Math.max(1, Math.floor(mipWidth / 2));
    mipHeight = Math.max(1, Math.floor(mipHeight / 2));
  }
  if (offset !== buffer.byteLength) throw new Error('DDS mip chain size mismatch');
  if (supports && !supports(format)) {
    const top = mipmaps[0];
    if ((dxgi === 71 || dxgi === 72) && top) {
      const texture = bc1Preview(top.data, width, height);
      texture.colorSpace = THREE.SRGBColorSpace;
      return texture;
    }
    throw new Error(`This GPU cannot preview DDS BC format ${dxgi}; its GLB still preserves the original blocks.`);
  }
  const texture = new THREE.CompressedTexture(mipmaps, width, height, format, THREE.UnsignedByteType);
  texture.colorSpace = [72, 75, 78, 99].includes(dxgi) || fourCC === 0x31545844 ? THREE.SRGBColorSpace : THREE.NoColorSpace;
  texture.magFilter = THREE.LinearFilter;
  texture.minFilter = count > 1 ? THREE.LinearMipmapLinearFilter : THREE.LinearFilter;
  texture.generateMipmaps = false;
  texture.flipY = false;
  texture.needsUpdate = true;
  return texture;
}

export function assetLoader(supports?: (format: THREE.CompressedPixelFormat) => boolean): GLTFLoader {
  return new GLTFLoader().register((parser: GLTFParser) => ({
    name: 'MSFT_texture_dds',
    loadTexture(textureIndex: number) {
      const document = object(parser.json);
      const texture = object(array(document['textures'])[textureIndex]);
      if (!texture['extensions']) return null;
      const extension = object(texture['extensions'])['MSFT_texture_dds'];
      if (!extension) return null;
      const image = object(array(document['images'])[index(object(extension)['source'])]);
      const bufferView = index(image['bufferView']);
      return parser.loadBufferView(bufferView).then((bytes) => ddsTexture(bytes, supports));
    },
  }));
}

export function loadGlb(buffer: ArrayBuffer, supports?: (format: THREE.CompressedPixelFormat) => boolean): Promise<GLTF> {
  return assetLoader(supports).parseAsync(buffer, '');
}

export function decodeGlb(base64: string): ArrayBuffer {
  const decoded = atob(base64);
  const bytes = new Uint8Array(decoded.length);
  for (let i = 0; i < decoded.length; i++) bytes[i] = decoded.charCodeAt(i);
  return bytes.buffer;
}
