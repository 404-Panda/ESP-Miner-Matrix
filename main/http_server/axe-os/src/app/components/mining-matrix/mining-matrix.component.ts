import { Component, OnDestroy, OnInit, ElementRef, ViewChild } from '@angular/core';
import { PersistenceService } from '../../persistence.service';

// DEV NOTE: Interfaces define the structure of data used throughout the component.
// These are critical for TypeScript type safety and should match the data coming from the WebSocket.
interface ParsedLine {
  raw: string;
  time: string;
  category: string;
  version: string;
  nonce: string;
  diff: number;
  diffMax: number;
  highlight: boolean;
  freq: number;
  jobId: string;
  core: string;
  prevBlockHash?: string;
  coinbase?: string;
  coinbase1?: string;
  coinbase2?: string;
  merkleBranches?: string[];
  ntime?: string;
  target?: string;
  username?: string;
  extranonce2?: string;
  stratumJobId?: string;
}

interface CoreInfo {
  coreId: string;
  lastNonce: string;
  lastDiff: number;
  highestDiff: number;
  lastDiffMax: number;
  lastTime: string;
}

interface ScatterPoint {
  x: number;
  y: number;
  color: string;
  radius: number;
}

interface MiningTask {
  jobId: string;
  coreId: string;
  version: string;
  prevBlockHash?: string;
  coinbase?: string;
  coinbase1?: string;
  coinbase2?: string;
  merkleBranches?: string[];
  ntime?: string;
  target?: string;
  nonce: string;
  difficulty: number;
  timestamp: number;
  username?: string;
  extranonce2?: string;
}

interface MiningNotify {
  jobId: string;
  prevBlockHash: string;
  coinbase1: string;
  coinbase2: string;
  merkleBranches: string[];
  version: string;
  target: string;
  ntime: string;
}

@Component({
  selector: 'app-mining-matrix',
  templateUrl: './mining-matrix.component.html',
  styleUrls: ['./mining-matrix.component.scss']
})
export class MiningMatrixComponent implements OnInit, OnDestroy {
  private ws?: WebSocket; // DEV NOTE: WebSocket connection for real-time log updates.
  public lines: ParsedLine[] = []; // DEV NOTE: Stores the most recent 100 log lines for display.
  public coreMap: Record<string, CoreInfo> = {}; // DEV NOTE: Tracks core performance data, persisted via PersistenceService.
  public bestCoreId: string | null = null; // DEV NOTE: ID of the core with the highest difficulty found.
  public bestDiff = 0; // DEV NOTE: Highest difficulty value encountered across all cores.
  public bestCoreDetailLines: string[] = []; // DEV NOTE: Raw log lines for the best core, capped at 50.
  private lastAsicResult: ParsedLine | null = null; // DEV NOTE: Temporary storage for the latest ASIC result, used to pair with jobInfo.
  public topCores: CoreInfo[] = []; // DEV NOTE: Top 5 cores by highest difficulty, updated dynamically.
  public miningTasks: MiningTask[] = []; // DEV NOTE: List of submitted shares, capped at 10, with highest difficulty at the top.
  private highestDiffTask: MiningTask | null = null; // DEV NOTE: Tracks the share with the highest difficulty for pinning at the top.
  private lastJob: { jobId: string; version: string; coreId: string; stratumJobId?: string } | null = null; // DEV NOTE: Last job info to pair with ASIC results.
  private notifyMap: Record<string, MiningNotify> = {}; // DEV NOTE: Maps job IDs to mining notify data for share enrichment.
  private jobIdMap: Record<string, string> = {}; // DEV NOTE: Unused currently, reserved for future job ID mapping if needed.

  public totalCores = 896; // DEV NOTE: Total number of BM1366 cores (112 big x 8 small).
  public gridSize = Math.ceil(Math.sqrt(this.totalCores)); // DEV NOTE: Grid size for heatmap display, calculated dynamically.
  public cores: { big: number; small: number }[] = []; // DEV NOTE: Array of core coordinates for heatmap rendering.
  public shareScatter: ScatterPoint[] = []; // DEV NOTE: Data points for the difficulty scatter chart, capped at 300.

  public chartData: any; // DEV NOTE: Configuration for the Chart.js scatter plot.
  public chartOptions: any; // DEV NOTE: Options for the Chart.js scatter plot, including logarithmic scale.

  @ViewChild('scrollContainer', { static: false })
  private scrollContainer?: ElementRef<HTMLDivElement>; // DEV NOTE: Reference to the log container for auto-scrolling.

  constructor(private persistenceService: PersistenceService) {} // DEV NOTE: Injected service for persisting coreMap and miningTasks.

  ngOnInit(): void {
    // DEV NOTE: Load persisted data on component initialization to maintain state across screen changes.
    this.coreMap = this.persistenceService.loadCoreMap();
    const { tasks, highestDiffTask } = this.persistenceService.loadMiningTasks();
    this.miningTasks = tasks;
    this.highestDiffTask = highestDiffTask;
    this.updateTopCores();
    this.recalculateBestCore();

    // DEV NOTE: Populate the cores array for the heatmap, representing all 896 cores (112 big x 8 small).
    for (let b = 0; b < 112; b++) {
      for (let s = 0; s < 8; s++) {
        this.cores.push({ big: b, small: s });
      }
    }

    // DEV NOTE: Establish WebSocket connection based on the current protocol (ws/wss).
    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const wsUrl = `${protocol}://${window.location.host}/api/ws`;
    this.ws = new WebSocket(wsUrl);

    // DEV NOTE: WebSocket event handlers for connection lifecycle and message handling.
    this.ws.onopen = () => console.log('WebSocket connected');
    this.ws.onmessage = (event) => {
      const text = event.data as string;
      console.log('WebSocket message received:', text);
      this.handleIncomingLog(text);
    };
    this.ws.onerror = (error) => console.error('WebSocket error:', error);
    this.ws.onclose = () => console.log('WebSocket closed');

    // DEV NOTE: Initialize Chart.js data for the scatter plot of share difficulties.
    this.chartData = {
      datasets: [{
        label: 'Difficulty',
        data: this.shareScatter,
        showLine: false,
        fill: false,
        pointRadius: (context: any) => this.shareScatter[context.dataIndex]?.radius || 3,
        backgroundColor: (context: any) => this.shareScatter[context.dataIndex]?.color || '#42A5F5'
      }]
    };

    // DEV NOTE: Chart options include a logarithmic y-axis for wide-ranging difficulties and time-based x-axis.
    this.chartOptions = {
      responsive: true,
      maintainAspectRatio: false,
      scales: {
        x: { type: 'time', time: { unit: 'second' }, title: { display: true, text: 'Time' } },
        y: {
          type: 'logarithmic',
          title: { display: true, text: 'Difficulty' },
          min: 100,
          ticks: {
            callback: (value: number) => {
              if (value >= 1e9) return `${value / 1e9}B`;
              if (value >= 1e6) return `${value / 1e6}M`;
              if (value >= 1e3) return `${value / 1e3}k`;
              return value;
            }
          }
        }
      }
    };
  }

  ngOnDestroy(): void {
    // DEV NOTE: Close WebSocket and save state to persistence when the component is destroyed (e.g., screen change).
    if (this.ws) this.ws.close();
    this.persistenceService.saveCoreMap(this.coreMap);
    this.persistenceService.saveMiningTasks(this.miningTasks, this.highestDiffTask);
  }

  private recalculateBestCore(): void {
    // DEV NOTE: Recalculate the best core based on highest difficulty in coreMap.
    this.bestDiff = 0;
    this.bestCoreId = null;
    for (const coreId in this.coreMap) {
      const core = this.coreMap[coreId];
      if (core.highestDiff > this.bestDiff) {
        this.bestDiff = core.highestDiff;
        this.bestCoreId = coreId;
      }
    }
  }

  private handleIncomingLog(rawLine: string): void {
    // DEV NOTE: Main entry point for processing incoming WebSocket messages.
    const parsed = this.parseLogLine(rawLine);
    this.lines.push(parsed);
    if (this.lines.length > 100) this.lines.shift(); // DEV NOTE: Cap log lines at 100 to prevent memory issues.

    // DEV NOTE: Reset all state on system restart, detected by a specific log message.
    if (rawLine.includes('Restarting System because of API Request')) {
      this.persistenceService.clearCoreMap();
      this.persistenceService.clearMiningTasks();
      this.coreMap = {};
      this.bestCoreId = null;
      this.bestDiff = 0;
      this.topCores = [];
      this.shareScatter = [];
      this.jobIdMap = {};
      this.notifyMap = {};
      this.miningTasks = [];
      this.highestDiffTask = null;
      this.chartData.datasets[0].data = this.shareScatter;
    }

    // DEV NOTE: Parse JSON-formatted mining.notify messages to populate notifyMap.
    try {
      const jsonData = JSON.parse(rawLine);
      if (jsonData.method === 'mining.notify' && jsonData.params) {
        const notify: MiningNotify = {
          jobId: jsonData.params[0],
          prevBlockHash: jsonData.params[1],
          coinbase1: jsonData.params[2],
          coinbase2: jsonData.params[3],
          merkleBranches: jsonData.params[4],
          version: jsonData.params[5],
          target: jsonData.params[6],
          ntime: jsonData.params[7]
        };
        this.notifyMap[notify.jobId] = notify;
        console.log('Mining notify received (JSON):', notify);
      }
    } catch (e) {
      // DEV NOTE: If not JSON, proceed with regular log parsing below.
    }

    this.updateCoreInfo(parsed);
    this.updateMiningTasks(parsed);

    // DEV NOTE: Log best core details for debugging, capped at 50 lines.
    if (this.bestCoreId && parsed.core === this.bestCoreId) {
      this.bestCoreDetailLines.push(rawLine);
      if (this.bestCoreDetailLines.length > 50) this.bestCoreDetailLines.shift();
    }

    // DEV NOTE: Scroll to the bottom of the log container with a slight delay to ensure DOM updates.
    setTimeout(() => this.scrollToBottom(), 20);
  }

  private updateMiningTasks(parsed: ParsedLine): void {
    // DEV NOTE: Handle job info, ASIC results, and share submissions to build the miningTasks list.
    if (parsed.category === 'jobInfo') {
      this.lastJob = { jobId: parsed.jobId, version: parsed.version, coreId: parsed.core };
      console.log('Last job updated:', this.lastJob);
    } else if (parsed.category === 'asic_result' && this.lastJob) {
      this.lastAsicResult = parsed;
      console.log('ASIC result stored temporarily:', parsed);
    } else if (parsed.category === 'share_submitted') {
      // DEV NOTE: Prevent duplicate shares within 1 second based on nonce.
      if (!this.miningTasks.some(t => t.nonce === parsed.nonce && t.timestamp > Date.now() - 1000)) {
        const task: MiningTask = {
          jobId: parsed.jobId,
          coreId: parsed.core || this.lastJob?.coreId || 'Unknown',
          version: parsed.version,
          nonce: parsed.nonce,
          difficulty: this.lastAsicResult ? this.lastAsicResult.diff : 0,
          timestamp: Date.now(),
          prevBlockHash: parsed.prevBlockHash,
          coinbase: parsed.coinbase,
          coinbase1: parsed.coinbase1,
          coinbase2: parsed.coinbase2,
          merkleBranches: parsed.merkleBranches,
          ntime: parsed.ntime,
          target: parsed.target,
          username: parsed.username,
          extranonce2: parsed.extranonce2
        };
        // DEV NOTE: Enrich task with data from notifyMap if available.
        const notify = this.notifyMap[task.jobId];
        if (notify) {
          task.prevBlockHash = notify.prevBlockHash || task.prevBlockHash;
          task.coinbase1 = notify.coinbase1 || task.coinbase1;
          task.coinbase2 = notify.coinbase2 || task.coinbase2;
          task.coinbase = notify.coinbase1 + notify.coinbase2 || task.coinbase;
          task.merkleBranches = notify.merkleBranches || task.merkleBranches;
          task.ntime = parsed.ntime || notify.ntime;
          task.target = notify.target || task.target;
        } else {
          console.warn(`No notify data found for job ${task.jobId} in notifyMap`);
        }

        // DEV NOTE: Update highestDiffTask if this task has a higher difficulty.
        if (!this.highestDiffTask || task.difficulty > this.highestDiffTask.difficulty) {
          this.highestDiffTask = { ...task }; // Clone to avoid reference issues
        }

        // DEV NOTE: Add new task and ensure highest difficulty task stays at the top.
        this.miningTasks.unshift(task);
        this.miningTasks = this.miningTasks.filter(t => t !== this.highestDiffTask); // Remove highest if it’s elsewhere
        this.miningTasks.unshift(this.highestDiffTask); // Add it back at the top
        
        // DEV NOTE: Limit to 10 tasks, including the highest difficulty one.
        if (this.miningTasks.length > 10) {
          this.miningTasks = this.miningTasks.slice(0, 10);
        }

        // DEV NOTE: Persist miningTasks and highestDiffTask to localStorage for screen persistence.
        this.persistenceService.saveMiningTasks(this.miningTasks, this.highestDiffTask);

        console.log('New mining task from share_submitted:', task);
        console.log('Updated miningTasks:', this.miningTasks);
      }
    }
  }

  private parseLogLine(rawLine: string): ParsedLine {
    // DEV NOTE: Parse raw log lines into structured ParsedLine objects based on content.
    const noAnsi = rawLine.replace(/\x1b\[[0-9;]*m/g, ''); // Remove ANSI color codes
    const time = new Date().toLocaleTimeString();

    const line: ParsedLine = {
      raw: rawLine,
      time,
      category: 'generic',
      version: '',
      nonce: '',
      diff: 0,
      diffMax: 0,
      highlight: false,
      freq: 0,
      jobId: '',
      core: '',
      prevBlockHash: undefined,
      coinbase: undefined,
      coinbase1: undefined,
      coinbase2: undefined,
      merkleBranches: undefined,
      ntime: undefined,
      target: undefined,
      username: undefined,
      extranonce2: undefined,
      stratumJobId: undefined
    };

    // DEV NOTE: Pattern matching for various log types; order matters for specificity.
    if (noAnsi.includes('Mining Notify - Job ID:')) {
      line.category = 'mining_notify';
      const jobIdMatch = /Job ID:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (jobIdMatch) line.jobId = jobIdMatch[1];
      const prevBlockMatch = /PrevBlockHash:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (prevBlockMatch) line.prevBlockHash = prevBlockMatch[1];
      const coinbase1Match = /Coinbase1:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (coinbase1Match) line.coinbase1 = coinbase1Match[1];
      const coinbase2Match = /Coinbase2:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (coinbase2Match) line.coinbase2 = coinbase2Match[1];
      const versionMatch = /Version:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (versionMatch) line.version = versionMatch[1];
      const targetMatch = /Target:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (targetMatch) line.target = targetMatch[1];
      const ntimeMatch = /Ntime:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (ntimeMatch) line.ntime = ntimeMatch[1];
      const merkleMatch = /Merkle Branches:\s*\[(.*?)\]/.exec(noAnsi);
      if (merkleMatch) {
        const branches = merkleMatch[1].split(',').map(b => b.trim());
        line.merkleBranches = branches.length > 0 && branches[0] !== '' ? branches : [];
      }
      if (line.jobId) {
        this.notifyMap[line.jobId] = {
          jobId: line.jobId,
          prevBlockHash: line.prevBlockHash || '',
          coinbase1: line.coinbase1 || '',
          coinbase2: line.coinbase2 || '',
          merkleBranches: line.merkleBranches || [],
          version: line.version || '',
          target: line.target || '',
          ntime: line.ntime || ''
        };
        console.log('Mining notify parsed from log:', this.notifyMap[line.jobId]);
      }
      return line;
    }

    if (noAnsi.includes('asic_result')) {
      line.category = 'asic_result';
      const verMatch = /Ver:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (verMatch) line.version = verMatch[1];
      const nonceMatch = /Nonce\s+([0-9A-Fa-f]+)/.exec(noAnsi);
      if (nonceMatch) line.nonce = nonceMatch[1];
      const diffMatch = /diff\s+([\d,.]+)\s+of\s+([\d,.]+)/.exec(noAnsi);
      if (diffMatch) {
        line.diff = parseFloat(diffMatch[1].replace(/,/g, ''));
        line.diffMax = parseFloat(diffMatch[2].replace(/,/g, ''));
        if (line.diff > line.diffMax) line.highlight = true;
      }
      return line;
    }

    if (noAnsi.includes('Setting Frequency to')) {
      line.category = 'frequency';
      const freqMatch = /Setting Frequency to\s+([\d.]+)MHz/.exec(noAnsi);
      if (freqMatch) line.freq = parseFloat(freqMatch[1]);
      return line;
    }

    if (noAnsi.includes('Send Job:')) {
      line.category = 'job';
      const jobMatch = /Send Job:\s+([0-9A-Fa-f]+)/.exec(noAnsi);
      if (jobMatch) line.jobId = jobMatch[1];
      return line;
    }

    if (noAnsi.includes('Job ID:')) {
      line.category = 'jobInfo';
      const jobIdMatch = /Job ID:\s+([0-9A-Fa-f]+)/.exec(noAnsi);
      if (jobIdMatch) line.jobId = jobIdMatch[1];
      const coreMatch = /Core:\s+(\d+\/\d+)/.exec(noAnsi);
      if (coreMatch) line.core = coreMatch[1];
      const verMatch = /Ver:\s+([0-9A-Fa-f]+)/.exec(noAnsi);
      if (verMatch) line.version = verMatch[1];
      return line;
    }

    if (noAnsi.includes('Share Submitted:')) {
      line.category = 'share_submitted';
      const coreMatch = /Core=([0-9]+\/[0-9]+)/.exec(noAnsi);
      if (coreMatch) line.core = coreMatch[1];
      const jobMatch = /Job=([0-9A-Fa-f]+)/.exec(noAnsi);
      if (jobMatch) line.jobId = jobMatch[1];
      const usernameMatch = /Username=([^,]+)/.exec(noAnsi);
      if (usernameMatch) line.username = usernameMatch[1];
      const extranonce2Match = /Extranonce2=([0-9A-Fa-f]+)/.exec(noAnsi);
      if (extranonce2Match) line.extranonce2 = extranonce2Match[1];
      const ntimeMatch = /ntime=([0-9A-Fa-f]+)/.exec(noAnsi);
      if (ntimeMatch) line.ntime = ntimeMatch[1];
      const nonceMatch = /Nonce=([0-9A-Fa-f]+)/.exec(noAnsi);
      if (nonceMatch) line.nonce = nonceMatch[1];
      const versionMatch = /Version=([0-9A-Fa-f]+)/.exec(noAnsi);
      if (versionMatch) line.version = versionMatch[1];
      const prevBlockMatch = /PrevBlock=([0-9A-Fa-f]+|N\/A)/.exec(noAnsi);
      if (prevBlockMatch && prevBlockMatch[1] !== 'N/A') line.prevBlockHash = prevBlockMatch[1];
      const coinbase1Match = /Coinbase1=([0-9A-Fa-f]+|N\/A)/.exec(noAnsi);
      if (coinbase1Match && coinbase1Match[1] !== 'N/A') line.coinbase1 = coinbase1Match[1];
      const coinbase2Match = /Coinbase2=([0-9A-Fa-f]+|N\/A)/.exec(noAnsi);
      if (coinbase2Match && coinbase2Match[1] !== 'N/A') line.coinbase2 = coinbase2Match[1];
      const merkleMatch = /Merkle=\[(.*?)\]/.exec(noAnsi);
      if (merkleMatch) {
        const branches = merkleMatch[1].split(',').map(b => b.trim());
        line.merkleBranches = branches.length > 0 && branches[0] !== '' ? branches : undefined;
      }
      const targetMatch = /Target=([0-9A-Fa-f]+|N\/A)/.exec(noAnsi);
      if (targetMatch && targetMatch[1] !== 'N/A') line.target = targetMatch[1];
      return line;
    }

    if (noAnsi.includes('"method": "mining.submit"')) {
      line.category = 'share_submitted';
      try {
        const jsonMatch = /{"id":.*?"mining.submit", "params": \[(.*?)\]}/.exec(noAnsi);
        if (jsonMatch) {
          const paramsStr = jsonMatch[1];
          const params = JSON.parse(`[${paramsStr}]`);
          line.username = params[0];
          line.jobId = params[1];
          line.extranonce2 = params[2];
          line.ntime = params[3];
          line.nonce = params[4];
          line.version = params[5];
          line.core = this.lastJob?.coreId || 'Unknown';
        }
      } catch (e) {
        console.error('Failed to parse mining.submit JSON:', e);
      }
      return line;
    }

    return line;
  }

  private getColorForDiff(diff: number, isBest: boolean): string {
    // DEV NOTE: Assigns colors to difficulty ranges for scatter plot visualization.
    if (diff <= 200) return '#4B0000';
    if (diff <= 500) return '#8B0000';
    if (diff <= 1000) return '#A30000';
    if (diff <= 5000) return '#CC0000';
    if (diff <= 10000) return '#FF3333';
    if (diff <= 50000) return '#006600';
    if (diff <= 100000) return '#008000';
    if (diff <= 500000) return '#00B300';
    if (diff <= 1000000) return '#33FF33';
    if (diff <= 5000000) return '#CCCC00';
    if (diff <= 10000000) return '#E6E600';
    if (diff <= 50000000) return '#FFFF00';
    if (diff <= 100000000) return '#FFFF66';
    if (diff <= 500000000) return '#FF9900';
    if (diff <= 1000000000) return '#FFAD33';
    if (diff <= 10000000000) return '#FFC107';
    return '#FFCC66';
  }

  private getRadiusForDiff(diff: number): number {
    // DEV NOTE: Calculates scatter point radius based on logarithmic difficulty for better visualization.
    const logDiff = Math.log10(diff);
    const minLog = Math.log10(100);
    const maxLog = Math.log10(1e11);
    const normalized = (logDiff - minLog) / (maxLog - minLog);
    return Math.max(3, Math.min(15, 3 + normalized * 12));
  }

  private updateCoreInfo(line: ParsedLine): void {
    // DEV NOTE: Updates coreMap with new ASIC results paired with jobInfo.
    if (line.category === 'asic_result') {
      this.lastAsicResult = line;
      return;
    }

    if (line.category === 'jobInfo' && line.core) {
      const coreId = line.core;
      if (this.lastAsicResult) {
        const diffVal = this.lastAsicResult.diff;
        const existingCore = this.coreMap[coreId];
        const highestDiff = existingCore ? Math.max(existingCore.highestDiff, diffVal) : diffVal;

        this.coreMap[coreId] = {
          coreId,
          lastNonce: this.lastAsicResult.nonce,
          lastDiff: diffVal,
          highestDiff,
          lastDiffMax: this.lastAsicResult.diffMax,
          lastTime: new Date().toLocaleTimeString()
        };

        this.updateTopCores();

        if (highestDiff > this.bestDiff) {
          this.bestDiff = highestDiff;
          this.bestCoreId = coreId;
          this.bestCoreDetailLines = [];
        }

        const isBest = coreId === this.bestCoreId;
        this.shareScatter.push({
          x: Date.now(),
          y: diffVal,
          color: this.getColorForDiff(highestDiff, isBest),
          radius: this.getRadiusForDiff(diffVal)
        });
        if (this.shareScatter.length > 300) this.shareScatter.shift(); // DEV NOTE: Cap scatter points at 300.

        this.lastAsicResult = null;
      } else {
        this.coreMap[coreId] = {
          coreId,
          lastNonce: '',
          lastDiff: 0,
          highestDiff: 0,
          lastDiffMax: 0,
          lastTime: ''
        };
      }
      this.persistenceService.saveCoreMap(this.coreMap);
    }
  }

  private updateTopCores(): void {
    // DEV NOTE: Maintains a list of the top 5 cores by highest difficulty.
    this.topCores = Object.values(this.coreMap)
      .sort((a, b) => b.highestDiff - a.highestDiff)
      .slice(0, 5);
  }

  public getCoreInfo(big: number, small: number): CoreInfo {
    // DEV NOTE: Retrieves or initializes core info for heatmap display.
    const key = `${big}/${small}`;
    return this.coreMap[key] || {
      coreId: key,
      lastNonce: '',
      lastDiff: 0,
      highestDiff: 0,
      lastDiffMax: 0,
      lastTime: ''
    };
  }

  public getCoreTitle(big: number, small: number): string {
    // DEV NOTE: Formats tooltip text for heatmap cells.
    const info = this.getCoreInfo(big, small);
    if (!info.lastNonce) return `Core ${info.coreId}\nNo Data Yet`;
    return `Core ${info.coreId}\nNonce=${info.lastNonce}\nHighest Diff=${info.highestDiff.toFixed(1)}\nLast Diff=${info.lastDiff.toFixed(1)}/${info.lastDiffMax.toFixed(1)}\nLastSeen=${info.lastTime}`;
  }

  public getCoreClass(big: number, small: number): string {
    // DEV NOTE: Determines CSS classes for heatmap cells based on difficulty and status.
    const info = this.getCoreInfo(big, small);
    const diff = info.highestDiff;
    const isBest = this.bestCoreId === info.coreId;
    const isTopCore = this.topCores.some(core => core.coreId === info.coreId);
    const isRecent = info.lastTime ? (new Date().getTime() - new Date(info.lastTime).getTime() < 5000) : false;

    if (!info.lastNonce) return 'cell-empty';

    let baseClass = '';
    if (diff <= 200) baseClass = 'cell-red1';
    else if (diff <= 500) baseClass = 'cell-red2';
    else if (diff <= 1000) baseClass = 'cell-red3';
    else if (diff <= 5000) baseClass = 'cell-red4';
    else if (diff <= 10000) baseClass = 'cell-red5';
    else if (diff <= 50000) baseClass = 'cell-green1';
    else if (diff <= 100000) baseClass = 'cell-green2';
    else if (diff <= 500000) baseClass = 'cell-green3';
    else if (diff <= 1000000) baseClass = 'cell-green4';
    else if (diff <= 5000000) baseClass = 'cell-yellow1';
    else if (diff <= 10000000) baseClass = 'cell-yellow2';
    else if (diff <= 50000000) baseClass = 'cell-yellow3';
    else if (diff <= 100000000) baseClass = 'cell-yellow4';
    else if (diff <= 500000000) baseClass = 'cell-orange1';
    else if (diff <= 1000000000) baseClass = 'cell-orange2';
    else if (diff <= 10000000000) baseClass = 'cell-orange3';
    else baseClass = 'cell-orange4';

    if (isBest) baseClass += ' best-core';
    if (isTopCore && isRecent) baseClass += ' updated';
    return baseClass;
  }

  public getBestCoreLabel(): string {
    // DEV NOTE: Displays the best core info in the UI.
    if (!this.bestCoreId) return 'No best core yet';
    return `Best Core: ${this.bestCoreId} (Diff=${this.bestDiff.toFixed(1)})`;
  }

  private scrollToBottom(): void {
    // DEV NOTE: Auto-scrolls the log container to the latest entry.
    if (this.scrollContainer?.nativeElement) {
      const el = this.scrollContainer.nativeElement;
      el.scrollTop = el.scrollHeight;
    }
  }

  public formatHex(hex: string | undefined, maxLength: number = 16): string {
    // DEV NOTE: Formats hex strings for display, truncating long values with ellipsis.
    if (!hex) return 'N/A';
    return hex.length > maxLength ? `${hex.slice(0, maxLength / 2)}...${hex.slice(-maxLength / 2)}` : hex;
  }

  public formatMerkleBranches(branches: string[] | undefined): string {
    // DEV NOTE: Formats Merkle branches into a comma-separated string for display.
    if (!branches || branches.length === 0) return '';
    return branches.map(b => this.formatHex(b, 16)).join(', ');
  }

  public trackByJobId(index: number, task: MiningTask): string {
    // DEV NOTE: Unique identifier for ngFor to optimize DOM updates.
    return `${task.jobId}-${task.coreId}-${task.timestamp}`;
  }

  public isNewTask(task: MiningTask): boolean {
    // DEV NOTE: Highlights tasks submitted within the last 2 seconds.
    return (Date.now() - task.timestamp) < 2000;
  }

  public isHighestDiffTask(task: MiningTask): boolean {
    // DEV NOTE: Identifies the highest difficulty task for special styling in the UI.
    return this.highestDiffTask === task;
  }
}
