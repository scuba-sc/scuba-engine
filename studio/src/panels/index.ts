import { homePanel } from './home';
import { registerPanel } from './registry';
import { viewportPanel } from './viewport';
import { widgetsPanel } from './widgets';
import { designerPanel } from './designer';
import { assetsPanel } from './assets';
import { netSandboxPanel } from './netsim';

registerPanel(homePanel);
registerPanel(viewportPanel);
registerPanel(widgetsPanel);
registerPanel(designerPanel);
registerPanel(assetsPanel);
registerPanel(netSandboxPanel);
