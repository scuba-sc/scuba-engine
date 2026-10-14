import { homePanel } from './home';
import { registerPanel } from './registry';
import { viewportPanel } from './viewport';
import { widgetsPanel } from './widgets';
import { designerPanel } from './designer';
import { assetsPanel } from './assets';

registerPanel(homePanel);
registerPanel(viewportPanel);
registerPanel(widgetsPanel);
registerPanel(designerPanel);
registerPanel(assetsPanel);
