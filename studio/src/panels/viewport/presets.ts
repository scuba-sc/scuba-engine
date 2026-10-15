export interface TerminalPreset {
  readonly id: string;
  readonly title: string;
  readonly category: string;
  readonly width: number;
  readonly height: number;
  readonly tilt: number;
  readonly heading: string;
  readonly summary: string;
  readonly actions: readonly string[];
}

export const terminalPresets: readonly TerminalPreset[] = [
  { id: 'elevator', title: 'Elevator keypad', category: 'ACCESS / 01', width: 1.05, height: 1.225, tilt: 0,
    heading: 'Select deck', summary: 'Transit shaft / synthetic fixture', actions: ['01', '02', '03', '04', '05', '06', '07', '08', '09'] },
  { id: 'asop', title: 'ASOP kiosk', category: 'HANGAR / 02', width: 1.5, height: 1.75, tilt: -0.08,
    heading: 'Fleet access', summary: 'Local simulation / no game connection', actions: ['Scout', 'Freighter', 'Shuttle', 'Retrieve', 'Store', 'Cancel'] },
  { id: 'shop', title: 'Shop terminal', category: 'COMMERCE / 03', width: 1.3, height: 1.5167, tilt: -0.18,
    heading: 'Supply depot', summary: 'Synthetic inventory / 3 items available', actions: ['Tool kit', 'Med kit', 'Battery', 'Inspect', 'Reserve', 'Clear'] },
  { id: 'engineering', title: 'Engineering console', category: 'SYSTEMS / 04', width: 1.7, height: 1.9833, tilt: -0.35,
    heading: 'Power routing', summary: 'Reactor nominal / simulated telemetry', actions: ['Life support', 'Drive', 'Shields', 'Balance', 'Isolate', 'Reset'] },
  { id: 'cockpit', title: 'Cockpit MFD', category: 'FLIGHT / 05', width: 1.25, height: 1.4583, tilt: -0.22,
    heading: 'Flight systems', summary: 'Training craft / all systems simulated', actions: ['Navigation', 'Comms', 'Sensors', 'Autopilot', 'Landing', 'Standby'] },
  { id: 'hud', title: 'Free-space HUD', category: 'OVERLAY / 06', width: 1.4, height: 1.6333, tilt: 0,
    heading: 'Spatial overlay', summary: 'World anchor / free-space projection', actions: ['Waypoint', 'Scan', 'Track', 'Mark', 'Range', 'Dismiss'] },
] as const;
