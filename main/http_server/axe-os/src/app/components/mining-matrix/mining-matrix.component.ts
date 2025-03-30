import { Component, OnDestroy, OnInit, ElementRef, ViewChild } from '@angular/core';
import { PersistenceService } from '../../persistence.service';

// Interface for parsed log lines from mining operations
interface ParsedLine {
  raw: string;          // Original unprocessed log line
  time: string;         // Timestamp of the log entry
  category: string;     // Type of log message (e.g., 'asic_result', 'jobInfo')
  version: string;      // Version information from the mining hardware
  nonce: string;        // Nonce value from mining result
  diff: number;         // Actual difficulty achieved
  diffMax: number;      // Maximum difficulty target
  highlight: boolean;   // Whether to highlight this entry (e.g., diff > diffMax)
  freq: number;         // Frequency setting in MHz
  jobId: string;        // Unique identifier for mining job
  core: string;         // Core identifier (e.g., "0/1" for big/small core)
}

// Interface for core-specific information tracking
interface CoreInfo {
  coreId: string;       // Unique identifier for the core (e.g., "0/1")
  lastNonce: string;    // Most recent nonce found by this core
  lastDiff: number;     // Most recent difficulty achieved
  highestDiff: number;  // Highest difficulty ever achieved by this core
  lastDiffMax: number;  // Most recent maximum difficulty target
  lastTime: string;     // Timestamp of last update
}

// Interface for scatter plot data points
interface ScatterPoint {
  x: number;           // Timestamp (x-axis)
  y: number;           // Difficulty value (y-axis)
  color: string;       // Color based on difficulty
  radius: number;      // Point size based on difficulty
}

// Interface for completed mining tasks
interface MiningTask {
  jobId: string;        // Unique job identifier
  coreId: string;       // Core that processed the task
  version: string;      // Mining software/hardware version
  prevBlockHash?: string; // Previous block hash (optional)
  coinbase?: string;    // Coinbase transaction data (optional)
  ntime?: string;       // Timestamp in mining format (optional)
  target?: string;      // Difficulty target (optional)
  nonce: string;        // Nonce solution
  difficulty: number;   // Achieved difficulty
  timestamp: number;    // Local timestamp of task completion
  username?: string;    // Miner username (optional)
  extranonce2?: string; // Extra nonce value (optional)
}

// Interface for mining.notify messages from the pool
interface MiningNotify {
  jobId: string;         // Job identifier
  prevBlockHash: string; // Previous block hash
  coinbase1: string;     // First part of coinbase transaction
  coinbase2: string;     // Second part of coinbase transaction
  merkleBranches: string[]; // Merkle branch data
  version: string;       // Block version
  target: string;        // Difficulty target
  ntime: string;         // Timestamp
}

@Component({
  selector: 'app-mining-matrix',
  templateUrl: './mining-matrix.component.html',
  styleUrls: ['./mining-matrix.component.scss']
})
export class MiningMatrixComponent implements OnInit, OnDestroy {
  // WebSocket connection for real-time updates
  private ws?: WebSocket;

  // Array of recent parsed log lines
  public lines: ParsedLine[] = [];
  
  // Map of core information by core ID
  public coreMap: Record<string, CoreInfo> = {};
  
  // ID of the core with highest difficulty
  public bestCoreId: string | null = null;
  
  // Highest difficulty achieved across all cores
  public bestDiff = 0;
  
  // Detailed log lines for the best core
  public bestCoreDetailLines: string[] = [];
  
  // Last ASIC result for pairing with job info
  private lastAsicResult: ParsedLine | null = null;
  
  // Top 5 cores by highest difficulty
  public topCores: CoreInfo[] = [];
  
  // Array of recent mining tasks
  public miningTasks: MiningTask[] = [];
  
  // Last job assignment information
  private lastJob: { jobId: string; version: string; coreId: string } | null = null;
  
  // Last mining.notify message received
  private lastNotify: MiningNotify | null = null;

  // Total number of cores in the mining system
  public totalCores = 896;
  
  // Size of the square grid for core visualization
  public gridSize = Math.ceil(Math.sqrt(this.totalCores));
  
  // Array of core coordinates (big/small indices)
  public cores: { big: number; small: number }[] = [];

  // Scatter plot data for share difficulty visualization
  public shareScatter: ScatterPoint[] = [];

  // Chart.js configuration objects
  public chartData: any;
  public chartOptions: any;

  // Reference to the scroll container element in the template
  @ViewChild('scrollContainer', { static: false })
  private scrollContainer?: ElementRef<HTMLDivElement>;

  constructor(private persistenceService: PersistenceService) {}

  /**
   * Initializes the component, setting up WebSocket connection and chart configuration
   */
  ngOnInit(): void {
    // Load persisted core data
    this.coreMap = this.persistenceService.loadCoreMap();
    this.updateTopCores();
    this.recalculateBestCore();

    // Generate core coordinate array (112 big cores × 8 small cores)
    for (let b = 0; b < 112; b++) {
      for (let s = 0; s < 8; s++) {
        this.cores.push({ big: b, small: s });
      }
    }

    // Establish WebSocket connection based on protocol
    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const wsUrl = `${protocol}://${window.location.host}/api/ws`;
    this.ws = new WebSocket(wsUrl);

    // WebSocket event handlers
    this.ws.onopen = () => console.log('WebSocket connected');
    
    this.ws.onmessage = (event) => {
      const text = event.data as string;
      console.log('WebSocket message received:', text);
      this.handleIncomingLog(text);
    };

    this.ws.onerror = (error) => console.error('WebSocket error:', error);
    this.ws.onclose = () => console.log('WebSocket closed');

    // Configure chart data for difficulty scatter plot
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

    // Configure chart options with logarithmic scale
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

  /**
   * Cleanup on component destruction
   */
  ngOnDestroy(): void {
    if (this.ws) this.ws.close();
    this.persistenceService.saveCoreMap(this.coreMap);
  }

  /**
   * Recalculates the best core based on highest difficulty
   */
  private recalculateBestCore(): void {
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

  /**
   * Handles incoming WebSocket messages and updates component state
   * @param rawLine The raw log message received
   */
  private handleIncomingLog(rawLine: string): void {
    const parsed = this.parseLogLine(rawLine);
    this.lines.push(parsed);
    if (this.lines.length > 100) this.lines.shift();

    // Handle system restart
    if (rawLine.includes('Restarting System because of API Request')) {
      this.persistenceService.clearCoreMap();
      this.coreMap = {};
      this.bestCoreId = null;
      this.bestDiff = 0;
      this.topCores = [];
      this.shareScatter = [];
      this.chartData.datasets[0].data = this.shareScatter;
    }

    // Parse JSON messages if applicable
    try {
      const jsonData = JSON.parse(rawLine);
      if (jsonData.method === 'mining.notify' && jsonData.params) {
        this.lastNotify = {
          jobId: jsonData.params[0],
          prevBlockHash: jsonData.params[1],
          coinbase1: jsonData.params[2],
          coinbase2: jsonData.params[3],
          merkleBranches: jsonData.params[4],
          version: jsonData.params[5],
          target: jsonData.params[6],
          ntime: jsonData.params[7]
        };
        console.log('Mining notify received:', this.lastNotify);
      } else if (jsonData.category === 'asic_result') {
        const task: MiningTask = {
          jobId: jsonData.jobId,
          coreId: this.lastJob ? this.lastJob.coreId : 'Unknown',
          version: jsonData.version,
          nonce: jsonData.nonce,
          difficulty: jsonData.diff,
          timestamp: Date.now(),
          username: jsonData.username,
          extranonce2: jsonData.extranonce2,
          ntime: jsonData.ntime
        };
        if (this.lastNotify && this.lastNotify.jobId === jsonData.jobId) {
          task.prevBlockHash = this.lastNotify.prevBlockHash;
          task.coinbase = this.lastNotify.coinbase1 + this.lastNotify.coinbase2;
          task.target = this.lastNotify.target;
        }
        this.miningTasks.unshift(task);
        console.log('New mining task from share result:', task);
        if (this.miningTasks.length > 5) this.miningTasks.pop();
      }
    } catch (e) {
      // Not a JSON message, continue with log parsing
    }

    this.updateCoreInfo(parsed);
    this.updateMiningTasks(parsed);

    if (this.bestCoreId && parsed.core === this.bestCoreId) {
      this.bestCoreDetailLines.push(rawLine);
      if (this.bestCoreDetailLines.length > 50) this.bestCoreDetailLines.shift();
    }

    setTimeout(() => this.scrollToBottom(), 20);
  }

  /**
   * Updates mining tasks based on parsed log line
   * @param parsed The parsed log line
   */
  private updateMiningTasks(parsed: ParsedLine): void {
    if (parsed.category === 'jobInfo') {
      this.lastJob = { jobId: parsed.jobId, version: parsed.version, coreId: parsed.core };
      console.log('Last job updated:', this.lastJob);
    } else if (parsed.category === 'asic_result' && this.lastJob) {
      if (!this.miningTasks.some(t => t.nonce === parsed.nonce && t.timestamp > Date.now() - 1000)) {
        const task: MiningTask = {
          jobId: this.lastJob.jobId,
          coreId: this.lastJob.coreId,
          version: this.lastJob.version,
          nonce: parsed.nonce,
          difficulty: parsed.diff,
          timestamp: Date.now()
        };
        if (this.lastNotify && this.lastNotify.jobId === this.lastJob.jobId) {
          task.prevBlockHash = this.lastNotify.prevBlockHash;
          task.coinbase = this.lastNotify.coinbase1 + this.lastNotify.coinbase2;
          task.ntime = this.lastNotify.ntime;
          task.target = this.lastNotify.target;
        }
        this.miningTasks.unshift(task);
        console.log('New mining task from log:', task);
        if (this.miningTasks.length > 5) this.miningTasks.pop();
      }
    }
  }

  /**
   * Parses a raw log line into a structured ParsedLine object
   * @param rawLine The raw log message
   * @returns ParsedLine object with extracted data
   */
  private parseLogLine(rawLine: string): ParsedLine {
    const noAnsi = rawLine.replace(/\x1b\[[0-9;]*m/g, '');
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
      core: ''
    };

    if (noAnsi.includes('asic_result')) {
      line.category = 'asic_result';
      const verMatch = /Ver:\s*([0-9A-Fa-f]+)/.exec(noAnsi);
      if (verMatch) line.version = verMatch[1];
      const nonceMatch = /Nonce\s+([0-9A-Fa-f]+)/.exec(noAnsi);
      if (nonceMatch) line.nonce = nonceMatch[1];
      const diffMatch = /diff\s+([\d.]+)\s+of\s+([\d.]+)/.exec(noAnsi);
      if (diffMatch) {
        line.diff = parseFloat(diffMatch[1]);
        line.diffMax = parseFloat(diffMatch[2]);
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

    return line;
  }

  /**
   * Determines color based on difficulty value
   * @param diff Difficulty value
   * @param isBest Whether this is the best core
   * @returns Hex color code
   */
  private getColorForDiff(diff: number, isBest: boolean): string {
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

  /**
   * Calculates point radius based on difficulty
   * @param diff Difficulty value
   * @returns Radius between 3 and 15
   */
  private getRadiusForDiff(diff: number): number {
    const logDiff = Math.log10(diff);
    const minLog = Math.log10(100);
    const maxLog = Math.log10(1e11);
    const normalized = (logDiff - minLog) / (maxLog - minLog);
    return Math.max(3, Math.min(15, 3 + normalized * 12));
  }

  /**
   * Updates core information based on parsed log data
   * @param line Parsed log line
   */
  private updateCoreInfo(line: ParsedLine): void {
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
        if (this.shareScatter.length > 300) this.shareScatter.shift();

        this.lastAsicResult = null;
      } else {
        this.coreMap[coreId] = {
          coreId,
          lastNonce: '',
          lastDiff: 0,
          highestDiff: 0,
          lastDiffMax: 0,
          lastTime: new Date().toLocaleTimeString()
        };
      }
      this.persistenceService.saveCoreMap(this.coreMap);
    }
  }

  /**
   * Updates the list of top 5 cores by highest difficulty
   */
  private updateTopCores(): void {
    this.topCores = Object.values(this.coreMap)
      .sort((a, b) => b.highestDiff - a.highestDiff)
      .slice(0, 5);
  }

  /**
   * Gets core information for a specific core
   * @param big Big core index
   * @param small Small core index
   * @returns CoreInfo object
   */
  public getCoreInfo(big: number, small: number): CoreInfo {
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

  /**
   * Generates tooltip text for a core
   * @param big Big core index
   * @param small Small core index
   * @returns Formatted string with core details
   */
  public getCoreTitle(big: number, small: number): string {
    const info = this.getCoreInfo(big, small);
    if (!info.lastNonce) return `Core ${info.coreId}\nNo Data Yet`;
    return `Core ${info.coreId}\nNonce=${info.lastNonce}\nHighest Diff=${info.highestDiff.toFixed(1)}\nLast Diff=${info.lastDiff.toFixed(1)}/${info.lastDiffMax.toFixed(1)}\nLastSeen=${info.lastTime}`;
  }

  /**
   * Determines CSS classes for core visualization
   * @param big Big core index
   * @param small Small core index
   * @returns Space-separated class string
   */
  public getCoreClass(big: number, small: number): string {
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

  /**
   * Gets label text for the best core
   * @returns Formatted string with best core info
   */
  public getBestCoreLabel(): string {
    if (!this.bestCoreId) return 'No best core yet';
    return `Best Core: ${this.bestCoreId} (Diff=${this.bestDiff.toFixed(1)})`;
  }

  /**
   * Scrolls the log container to the bottom
   */
  private scrollToBottom(): void {
    if (this.scrollContainer?.nativeElement) {
      const el = this.scrollContainer.nativeElement;
      el.scrollTop = el.scrollHeight;
    }
  }

  /**
   * Formats hex strings with truncation if needed
   * @param hex Hex string to format
   * @param maxLength Maximum length before truncation
   * @returns Formatted hex string
   */
  public formatHex(hex: string | undefined, maxLength: number = 16): string {
    if (!hex) return 'N/A';
    return hex.length > maxLength ? `${hex.slice(0, maxLength / 2)}...${hex.slice(-maxLength / 2)}` : hex;
  }

  /**
   * Tracking function for ngFor over mining tasks
   * @param index Item index
   * @param task Mining task object
   * @returns Unique identifier
   */
  public trackByJobId(index: number, task: MiningTask): string {
    return `${task.jobId}-${task.coreId}-${task.timestamp}`;
  }

  /**
   * Determines if a task is recent (within 2 seconds)
   * @param task Mining task object
   * @returns Boolean indicating if task is new
   */
  public isNewTask(task: MiningTask): boolean {
    return (Date.now() - task.timestamp) < 2000;
  }
}
