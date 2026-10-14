import { homePanel } from './home';
import { registerPanel } from './registry';
import { viewportPanel } from './viewport';
import { netSandboxPanel } from './netsim';

registerPanel(homePanel);
registerPanel(viewportPanel);
registerPanel(netSandboxPanel);
