import type { PanelModule } from './registry';

// Placeholder 3D stage: proves the Three.js pipeline; the diegetic stage replaces it. Three.js is
// loaded on first use so the shell and Home panel stay small.
export const viewportPanel: PanelModule = {
  id: 'viewport',
  title: 'Viewport',
  order: 10,
  mount(container) {
    let cleanup: (() => void) | undefined;
    let hidden = false;

    void import('three').then((THREE) => {
      if (hidden) return;
      let renderer: InstanceType<typeof THREE.WebGLRenderer>;
      try {
        renderer = new THREE.WebGLRenderer({ antialias: true });
      } catch {
        container.textContent = 'WebGL is not available in this browser.';
        return;
      }
      const scene = new THREE.Scene();
      const camera = new THREE.PerspectiveCamera(50, 1, 0.1, 100);
      camera.position.set(3, 2.5, 4);
      camera.lookAt(0, 0, 0);
      scene.add(new THREE.AmbientLight(0xffffff, 0.4));
      const sun = new THREE.DirectionalLight(0xffffff, 1.2);
      sun.position.set(5, 8, 3);
      scene.add(sun);
      scene.add(new THREE.GridHelper(10, 10, 0x335577, 0x223344));
      const cube = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), new THREE.MeshStandardMaterial({ color: 0x2a9df4 }));
      cube.position.y = 0.5;
      scene.add(cube);

      renderer.domElement.className = 'viewport-canvas';
      container.append(renderer.domElement);

      const resize = () => {
        const w = Math.max(container.clientWidth, 1);
        const h = Math.max(container.clientHeight, 1);
        renderer.setSize(w, h, false);
        camera.aspect = w / h;
        camera.updateProjectionMatrix();
      };
      const observer = new ResizeObserver(resize);
      observer.observe(container);
      resize();

      renderer.setAnimationLoop((time: number) => {
        cube.rotation.y = time / 1000;
        renderer.render(scene, camera);
      });

      cleanup = () => {
        renderer.setAnimationLoop(null);
        observer.disconnect();
        cube.geometry.dispose();
        cube.material.dispose();
        renderer.dispose();
        renderer.domElement.remove();
      };
    });

    return () => {
      hidden = true;
      cleanup?.();
    };
  },
};
