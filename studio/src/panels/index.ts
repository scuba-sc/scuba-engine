import { homePanel } from './home';
import { registerPanel } from './registry';
import { viewportPanel } from './viewport';
import { assetsPanel } from './assets';

registerPanel(homePanel);
registerPanel(viewportPanel);
registerPanel(assetsPanel);
