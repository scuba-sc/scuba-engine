import './style.css';
import './panels/index';
import { HostdBridge } from './bridge';
import { createShell } from './shell';

const root = document.getElementById('app');
if (!root) throw new Error('#app missing from index.html');

const bridge = new HostdBridge({ url: import.meta.env.VITE_HOSTD_URL || undefined });
createShell(root, bridge);
bridge.connect();
