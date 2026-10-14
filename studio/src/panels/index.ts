import { homePanel } from './home';
import { registerPanel } from './registry';
import { viewportPanel } from './viewport';
import { widgetsPanel } from './widgets';

registerPanel(homePanel);
registerPanel(viewportPanel);
registerPanel(widgetsPanel);
