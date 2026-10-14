import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { Mesh, MeshStandardMaterial, CompressedTexture, DataTexture } from 'three';
import { ddsTexture, loadGlb } from './loader';

const fixtures = resolve(import.meta.dirname, '../../../../build/tests/assets-fixtures');
function bytes(name: string): ArrayBuffer {
  return new Uint8Array(readFileSync(resolve(fixtures, name))).buffer;
}

describe('native asset output in the studio GLTFLoader', () => {
  it('loads a converted synthetic Cry triangle with correct vertices and bounds', async () => {
    const gltf = await loadGlb(bytes('triangle.glb'));
    const meshes: Mesh[] = [];
    gltf.scene.traverse((object) => { if (object instanceof Mesh) meshes.push(object); });
    expect(meshes).toHaveLength(1);
    const mesh = meshes[0];
    if (!mesh) throw new Error('Expected converted triangle mesh');
    expect(mesh.geometry.index?.array).toEqual(new Uint32Array([0, 1, 2]));
    expect(Array.from(mesh.geometry.getAttribute('position').array)).toEqual([0, 0, 0, 1, 0, 0, 0, 1, 0]);
    mesh.geometry.computeBoundingBox();
    expect(mesh.geometry.boundingBox?.max.toArray()).toEqual([1, 1, 0]);
  });

  it('loads a DDS textured GLB without network images or software BC decoding', async () => {
    const gltf = await loadGlb(bytes('texture.glb'));
    const textures: CompressedTexture[] = [];
    gltf.scene.traverse((object) => {
      if (object instanceof Mesh && object.material instanceof MeshStandardMaterial &&
          object.material.map instanceof CompressedTexture) textures.push(object.material.map);
    });
    expect(textures).toHaveLength(1);
    const texture = textures[0];
    if (!texture) throw new Error('Expected embedded DDS texture');
    expect(texture.image.width).toBe(4);
    expect(texture.image.height).toBe(4);
    expect(texture.mipmaps).toHaveLength(1);
    expect(texture.mipmaps[0]?.data.byteLength).toBe(8);
  });

  it('refuses every truncation of a DDS before creating texture views', () => {
    const input = bytes('red.dds');
    for (let length = 0; length < input.byteLength; length++) expect(() => ddsTexture(input.slice(0, length))).toThrow();
  });

  it('reports unavailable compressed-texture GPU support instead of showing a blank model', () => {
    expect(() => ddsTexture(bytes('bc-99.dds'), () => false)).toThrow();
  });

  it('decodes the red BC1 fixture portably when the GPU lacks S3TC', () => {
    const texture = ddsTexture(bytes('red.dds'), () => false);
    expect(texture).toBeInstanceOf(DataTexture);
    if (!(texture instanceof DataTexture) || !texture.image.data) throw new Error('Expected portable BC1 image');
    expect(Array.from(texture.image.data.slice(0, 4))).toEqual([255, 0, 0, 255]);
    expect(texture.image.data.length).toBe(64);
    texture.dispose();
  });

  it('interpolates BC1 selectors and retains transparent pixels on partial blocks', () => {
    const buffer = bytes('red.dds');
    const header = new DataView(buffer);
    header.setUint32(12, 1, true);
    header.setUint32(16, 1, true);
    header.setUint32(132, 0xaaaaaaaa, true);
    const interpolated = ddsTexture(buffer, () => false);
    if (!(interpolated instanceof DataTexture) || !interpolated.image.data) throw new Error('Expected portable BC1 image');
    expect(Array.from(interpolated.image.data)).toEqual([170, 85, 0, 255]);
    header.setUint16(128, 0, true);
    header.setUint16(130, 0, true);
    header.setUint32(132, 0xffffffff, true);
    const transparent = ddsTexture(buffer, () => false);
    if (!(transparent instanceof DataTexture) || !transparent.image.data) throw new Error('Expected portable BC1 image');
    expect(Array.from(transparent.image.data)).toEqual([0, 0, 0, 0]);
    interpolated.dispose();
    transparent.dispose();
  });

  it('preserves every supported DX10 BC format and its complete mip payload', () => {
    for (const dxgi of [71, 72, 74, 75, 77, 78, 80, 81, 83, 84, 95, 96, 98, 99]) {
      const texture = ddsTexture(bytes(`bc-${dxgi}.dds`));
      if (!(texture instanceof CompressedTexture)) throw new Error('Expected compressed BC image');
      expect(texture.image.width).toBe(4);
      expect(texture.mipmaps).toHaveLength(1);
      expect(texture.mipmaps[0]?.data.byteLength).toBe([71, 72, 80, 81].includes(dxgi) ? 8 : 16);
      texture.dispose();
    }
  });

  it('loads a custom OBJ import from the emitted VFS pack layout', async () => {
    const gltf = await loadGlb(bytes('imported-pack/files/objects/synthetic.glb'));
    let vertices = 0;
    gltf.scene.traverse((object) => { if (object instanceof Mesh) vertices += object.geometry.getAttribute('position').count; });
    expect(vertices).toBe(3);
  });
});
