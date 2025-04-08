import { Component, OnDestroy, OnInit, ElementRef, ViewChild, HostListener } from '@angular/core';
import { PersistenceService } from '../../persistence.service';
import { Chart } from 'chart.js';

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
  patoshiRange?: string;
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

interface PatoshiTracker {
  coreId: string;
  bestNonce: string;
  rangeStart: number;
  rangeEnd: number;
  nonceCount: number;
  coreIndex: number; // New field for numeric x-axis
  rangeIndex: number; // New field for numeric y-axis
}

interface PatoshiRange {
  start: number;
  end: number;
  label: string;
  color: string;
}

@Component({
  selector: 'app-mining-matrix',
  templateUrl: './mining-matrix.component.html',
  styleUrls: ['./mining-matrix.component.scss']
})
export class MiningMatrixComponent implements OnInit, OnDestroy {
  private ws?: WebSocket;
  public lines: ParsedLine[] = [];
  public coreMap: Record<string, CoreInfo> = {};
  public bestCoreId: string | null = null;
  public bestDiff = 0;
  public bestCoreDetailLines: string[] = [];
  private lastAsicResult: ParsedLine | null = null;
  public topCores: CoreInfo[] = [];
  public miningTasks: MiningTask[] = [];
  private highestDiffTask: MiningTask | null = null;
  private lastJob: { jobId: string; version: string; coreId: string; stratumJobId?: string } | null = null;
  private notifyMap: Record<string, MiningNotify> = {};
  private jobIdMap: Record<string, string> = {};

  public totalCores = 896; // Initial estimate, will adjust dynamically
  public gridSize = Math.ceil(Math.sqrt(this.totalCores));
  public cores: { big: number; small: number }[] = [];
  public shareScatter: ScatterPoint[] = [];
  public patoshiTrackers: PatoshiTracker[] = [];
  public patoshiChartData: any;
  public patoshiChartOptions: any;
  public chartData: any;
  public chartOptions: any;

  public trackerHeight: number = 200;
  public matrixHeight: number = 200; // New for matrix-container
  public bestCoreLogsHeight: number = 150; // New for best-core-logs
  public pipelineHeight: number = 500; // New for mining-pipeline
  public isResizing: boolean = false;
  public resizingSection: string | null = null; // Track which section is being resized
  public trackerScale: number = 1.0; // Added missing property
  public selectedRange: string | null = null; // Added missing property


  public patoshiRanges: PatoshiRange[] = [
    { start: 0, end: 163840000, label: '0', color: '#FF6384' },
    { start: 163840000, end: 327680000, label: '1', color: '#FF8A65' },
    { start: 327680000, end: 491520000, label: '2', color: '#FFCA28' },
    { start: 491520000, end: 655360000, label: '3', color: '#FFD54F' },
    { start: 655360000, end: 819200000, label: '4', color: '#FFF176' },
    { start: 819200000, end: 983040000, label: '5', color: '#DCE775' },
    { start: 983040000, end: 1146880000, label: '6', color: '#AED581' },
    { start: 1146880000, end: 1310720000, label: '7', color: '#81C784' },
    { start: 1310720000, end: 1474560000, label: '8', color: '#4CAF50' },
    { start: 1474560000, end: 1638400000, label: '9', color: '#388E3C' },
    { start: 1638400000, end: 1802240000, label: '10', color: '#B0BEC5' },
    { start: 1802240000, end: 1966080000, label: '11', color: '#CFD8DC' },
    { start: 1966080000, end: 2129920000, label: '12', color: '#ECEFF1' },
    { start: 2129920000, end: 2293760000, label: '13', color: '#B0BEC5' },
    { start: 2293760000, end: 2457600000, label: '14', color: '#CFD8DC' },
    { start: 2457600000, end: 2621440000, label: '15', color: '#ECEFF1' },
    { start: 2621440000, end: 2785280000, label: '16', color: '#B0BEC5' },
    { start: 2785280000, end: 2949120000, label: '17', color: '#CFD8DC' },
    { start: 2949120000, end: 3112960000, label: '18', color: '#ECEFF1' },
    { start: 3112960000, end: 3276800000, label: '19', color: '#36A2EB' },
    { start: 3276800000, end: 3440640000, label: '20', color: '#4FC3F7' },
    { start: 3440640000, end: 3604480000, label: '21', color: '#81D4FA' },
    { start: 3604480000, end: 3768320000, label: '22', color: '#B3E5FC' },
    { start: 3768320000, end: 3932160000, label: '23', color: '#E1F5FE' },
    { start: 3932160000, end: 4096000000, label: '24', color: '#BA68C8' },
    { start: 4096000000, end: 4259840000, label: '25', color: '#CE93D8' },
    { start: 4259840000, end: 4423680000, label: '26', color: '#F06292' },
    { start: 4423680000, end: 4587520000, label: '27', color: '#F48FB1' },
    { start: 4587520000, end: 4751360000, label: '28', color: '#F8BBD0' },
    { start: 4751360000, end: 4294967295, label: '29', color: '#B0BEC5' }
  ];

  @ViewChild('scrollContainer', { static: false }) private scrollContainer?: ElementRef<HTMLDivElement>;
  @ViewChild('patoshiTracker', { static: false }) patoshiTrackerRef?: ElementRef<HTMLDivElement>;
  @ViewChild('patoshiChart', { static: false }) patoshiChart?: ElementRef<HTMLCanvasElement>;
  @ViewChild('bestCoreLogs', { static: false }) bestCoreLogsRef?: ElementRef<HTMLDivElement>; // New
  @ViewChild('pipelineContainer', { static: false }) pipelineContainerRef?: ElementRef<HTMLDivElement>; // New

  constructor(private persistenceService: PersistenceService) {}

  ngOnInit(): void {
    this.coreMap = this.persistenceService.loadCoreMap();
    const { tasks, highestDiffTask } = this.persistenceService.loadMiningTasks();
    this.miningTasks = tasks;
    this.highestDiffTask = highestDiffTask;
    this.updateTopCores();
    this.recalculateBestCore();

    // Dynamically adjust core list based on observed core IDs
    this.cores = [];
    for (let b = 0; b < 120; b++) { // Increased to 120 to cover >900 cores
      for (let s = 0; s < 8; s++) {
        this.cores.push({ big: b, small: s });
      }
    }
    this.totalCores = this.cores.length; // Update totalCores dynamically
    this.gridSize = Math.ceil(Math.sqrt(this.totalCores));

    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const wsUrl = `${protocol}://${window.location.host}/api/ws`;
    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => console.log('WebSocket connected');
    this.ws.onmessage = (event) => {
      const text = event.data as string;
      console.log('WebSocket message received:', text);
      this.handleIncomingLog(text);
    };
    this.ws.onerror = (error) => console.error('WebSocket error:', error);
    this.ws.onclose = () => console.log('WebSocket closed');

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

    this.patoshiChartData = {
      datasets: [{
        label: 'Patoshi Range Hits',
        data: [],
        backgroundColor: (context: any) => {
          const tracker = this.patoshiTrackers[context.dataIndex];
          const range = this.patoshiRanges.find(r => tracker?.rangeStart === r.start && tracker?.rangeEnd === r.end);
          return range ? range.color : '#FF6384';
        },
        borderColor: '#FF6384',
        pointRadius: 5
      }]
    };

    this.patoshiChartOptions = {
      responsive: true,
      maintainAspectRatio: false,
      scales: {
        x: {
          type: 'linear', // Changed to linear for core index
          title: { display: true, text: 'Core Index' },
          min: 0,
          max: this.totalCores - 1,
          ticks: {
            stepSize: 50, // Show every 50th core index for readability
            callback: (value: number) => {
              const core = this.cores[value];
              return core ? `${core.big}/${core.small}` : value;
            }
          }
        },
        y: {
          type: 'linear', // Numeric y-axis for range index
          title: { display: true, text: 'Patoshi Range' },
          min: 0,
          max: this.patoshiRanges.length - 1,
          ticks: {
            stepSize: 1,
            callback: (value: number) => this.patoshiRanges[value]?.label || value
          }
        }
      },
      plugins: {
        zoom: {
          pan: {
            enabled: true,
            mode: 'x' // Allow horizontal panning
          },
          zoom: {
            wheel: {
              enabled: true // Enable zoom with mouse wheel
            },
            pinch: {
              enabled: true // Enable pinch zoom on touch devices
            },
            mode: 'x' // Zoom only on x-axis
          }
        },
        tooltip: {
          callbacks: {
            label: (context: any) => {
              const tracker = this.patoshiTrackers[context.dataIndex];
              const core = this.cores[tracker.coreIndex];
              const range = this.patoshiRanges[tracker.rangeIndex];
              return tracker ? `Core: ${core.big}/${core.small}, Range: ${range.label}, Nonce: ${tracker.bestNonce}` : '';
            }
          }
        }
      }
    };

    console.log('Patoshi chart initialized with data:', this.patoshiChartData);
  }

  ngOnDestroy(): void {
    if (this.ws) this.ws.close();
    this.persistenceService.saveCoreMap(this.coreMap);
    this.persistenceService.saveMiningTasks(this.miningTasks, this.highestDiffTask);
  }

  startResize(event: MouseEvent, section: string): void {
    this.isResizing = true;
    this.resizingSection = section;
    event.preventDefault();
  }

  @HostListener('document:mousemove', ['$event'])
  onMouseMove(event: MouseEvent): void {
    if (!this.isResizing || !this.resizingSection) return;

    let ref: ElementRef<HTMLDivElement> | undefined;
    let heightProp: 'trackerHeight' | 'matrixHeight' | 'bestCoreLogsHeight' | 'pipelineHeight';

    switch (this.resizingSection) {
      case 'patoshi':
        ref = this.patoshiTrackerRef;
        heightProp = 'trackerHeight';
        break;
      case 'matrix':
        ref = this.scrollContainer;
        heightProp = 'matrixHeight';
        break;
      case 'bestCoreLogs':
        ref = this.bestCoreLogsRef;
        heightProp = 'bestCoreLogsHeight';
        break;
      case 'pipeline':
        ref = this.pipelineContainerRef;
        heightProp = 'pipelineHeight';
        break;
      default:
        return;
    }

    if (ref) {
      const newHeight = event.clientY - ref.nativeElement.getBoundingClientRect().top;
      this[heightProp] = Math.max(100, Math.min(600, newHeight));
    }
  }

  @HostListener('document:mouseup')
  onMouseUp(): void {
    this.isResizing = false;
    this.resizingSection = null;
  }

  scaleTracker(delta: number): void {
    this.trackerScale = Math.max(0.5, Math.min(2.0, this.trackerScale + delta));
  }

  toggleRangeFilter(event: Event): void {
    const target = event.target as HTMLSelectElement;
    this.selectedRange = target.value === '' ? null : target.value;
  }

  getFilteredTrackers(): PatoshiTracker[] {
    if (!this.selectedRange) return this.patoshiTrackers;
    const range = this.patoshiRanges.find(r => r.label === this.selectedRange);
    return range ? this.patoshiTrackers.filter(t => t.rangeStart === range.start && t.rangeEnd === range.end) : this.patoshiTrackers;
  }

  getTrackerBorderColor(tracker: PatoshiTracker): string {
    const range = this.patoshiRanges.find(r => r.start === tracker.rangeStart && r.end === tracker.rangeEnd);
    return range ? range.color : '#666';
  }

  formatRangeValue(value: number): string {
    return (value / 1e6).toFixed(1) + 'M';
  }

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

  private handleIncomingLog(rawLine: string): void {
    const parsed = this.parseLogLine(rawLine);
    this.lines.push(parsed);
    if (this.lines.length > 100) this.lines.shift();

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
      this.patoshiTrackers = [];
      this.chartData.datasets[0].data = this.shareScatter;
      this.patoshiChartData.datasets[0].data = [];
    }

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
    } catch (e) {}

    this.updateCoreInfo(parsed);
    this.updateMiningTasks(parsed);
    this.updatePatoshiTracker(rawLine);

    if (this.bestCoreId && parsed.core === this.bestCoreId) {
      this.bestCoreDetailLines.push(rawLine);
      if (this.bestCoreDetailLines.length > 50) this.bestCoreDetailLines.shift();
    }

    setTimeout(() => this.scrollToBottom(), 20);
  }

  private updatePatoshiTracker(rawLine: string): void {
    const rangeMatch = /Range hit: Core (\d+), Nonce (\d+), Range (\d+) \[(\d+)-(\d+)\], Patoshi: (\d+)/.exec(rawLine);
    if (rangeMatch) {
      const coreBig = parseInt(rangeMatch[1]);
      const nonce = rangeMatch[2];
      const rangeIndex = parseInt(rangeMatch[3]);
      const rangeStart = parseInt(rangeMatch[4]);
      const rangeEnd = parseInt(rangeMatch[5]);
      const isPatoshi = parseInt(rangeMatch[6]);
      const coreId = `${coreBig}/0`;
      const coreIndex = this.cores.findIndex(c => c.big === coreBig && c.small === 0);

      if (coreIndex === -1) {
        console.error(`Core ${coreId} not found in this.cores`);
        return;
      }

      if (coreIndex >= this.totalCores) {
        this.totalCores = coreIndex + 1;
        this.patoshiChartOptions.scales.x.max = this.totalCores - 1;
        console.log(`Adjusted totalCores to ${this.totalCores}`);
      }

      const existing = this.patoshiTrackers.find(t => t.coreId === coreId);
      if (existing) {
        existing.bestNonce = nonce;
        existing.rangeStart = rangeStart;
        existing.rangeEnd = rangeEnd;
        existing.nonceCount++;
        existing.rangeIndex = rangeIndex;
      } else {
        this.patoshiTrackers.push({
          coreId,
          bestNonce: nonce,
          rangeStart,
          rangeEnd,
          nonceCount: 1,
          coreIndex,
          rangeIndex
        });
      }

      const updatedData = this.patoshiTrackers.map(t => ({
        x: t.coreIndex,
        y: t.rangeIndex
      }));

      this.patoshiChartData = {
        datasets: [{
          label: 'Range Hits',
          data: updatedData,
          backgroundColor: (context: any) => {
            const tracker = this.patoshiTrackers[context.dataIndex];
            const range = this.patoshiRanges.find(r => tracker?.rangeStart === r.start && tracker?.rangeEnd === r.end);
            return range ? range.color : '#FF6384';
          },
          borderColor: '#FF6384',
          pointRadius: 5
        }]
      };
    }
  }
  
  private updateMiningTasks(parsed: ParsedLine): void {
    if (parsed.category === 'jobInfo') {
      this.lastJob = { jobId: parsed.jobId, version: parsed.version, coreId: parsed.core };
      console.log('Last job updated:', this.lastJob);
    } else if (parsed.category === 'asic_result' && this.lastJob) {
      this.lastAsicResult = parsed;
      console.log('ASIC result stored temporarily:', parsed);
    } else if (parsed.category === 'share_submitted') {
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

        const nonceNum = parseInt(task.nonce, 16);
        const range = this.patoshiRanges.find(r => nonceNum >= r.start && nonceNum <= r.end);
        if (range) task.patoshiRange = range.label;

        const notify = this.notifyMap[task.jobId];
        if (notify) {
          task.prevBlockHash = notify.prevBlockHash || task.prevBlockHash;
          task.coinbase1 = notify.coinbase1 || task.coinbase1;
          task.coinbase2 = notify.coinbase2 || task.coinbase2;
          task.coinbase = notify.coinbase1 + notify.coinbase2 || task.coinbase;
          task.merkleBranches = notify.merkleBranches || task.merkleBranches;
          task.ntime = parsed.ntime || notify.ntime;
          task.target = notify.target || task.target;
        }

        if (!this.highestDiffTask || task.difficulty > this.highestDiffTask.difficulty) {
          this.highestDiffTask = { ...task };
        }

        this.miningTasks.unshift(task);
        this.miningTasks = this.miningTasks.filter(t => t !== this.highestDiffTask);
        this.miningTasks.unshift(this.highestDiffTask);
        
        if (this.miningTasks.length > 10) {
          this.miningTasks = this.miningTasks.slice(0, 10);
        }

        this.persistenceService.saveMiningTasks(this.miningTasks, this.highestDiffTask);

        console.log('New mining task from share_submitted:', task);
        console.log('Updated miningTasks:', this.miningTasks);
      }
    }
  }

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
    const logDiff = Math.log10(diff);
    const minLog = Math.log10(100);
    const maxLog = Math.log10(1e11);
    const normalized = (logDiff - minLog) / (maxLog - minLog);
    return Math.max(3, Math.min(15, 3 + normalized * 12));
  }

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
          lastTime: ''
        };
      }
      this.persistenceService.saveCoreMap(this.coreMap);
    }
  }

  private updateTopCores(): void {
    this.topCores = Object.values(this.coreMap)
      .sort((a, b) => b.highestDiff - a.highestDiff)
      .slice(0, 5);
  }

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

  public getCoreTitle(big: number, small: number): string {
    const info = this.getCoreInfo(big, small);
    if (!info.lastNonce) return `Core ${info.coreId}\nNo Data Yet`;
    return `Core ${info.coreId}\nNonce=${info.lastNonce}\nHighest Diff=${info.highestDiff.toFixed(1)}\nLast Diff=${info.lastDiff.toFixed(1)}/${info.lastDiffMax.toFixed(1)}\nLastSeen=${info.lastTime}`;
  }

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

  public getBestCoreLabel(): string {
    if (!this.bestCoreId) return 'No best core yet';
    return `Best Core: ${this.bestCoreId} (Diff=${this.bestDiff.toFixed(1)})`;
  }

  private scrollToBottom(): void {
    if (this.scrollContainer?.nativeElement) {
      const el = this.scrollContainer.nativeElement;
      el.scrollTop = el.scrollHeight;
    }
  }

  public formatHex(hex: string | undefined, maxLength: number = 16): string {
    if (!hex) return 'N/A';
    return hex.length > maxLength ? `${hex.slice(0, maxLength / 2)}...${hex.slice(-maxLength / 2)}` : hex;
  }

  public formatMerkleBranches(branches: string[] | undefined): string {
    if (!branches || branches.length === 0) return '';
    return branches.map(b => this.formatHex(b, 16)).join(', ');
  }

  public trackByJobId(index: number, task: MiningTask): string {
    return `${task.jobId}-${task.coreId}-${task.timestamp}`;
  }

  public isNewTask(task: MiningTask): boolean {
    return (Date.now() - task.timestamp) < 2000;
  }

  public isHighestDiffTask(task: MiningTask): boolean {
    return this.highestDiffTask === task;
  }
}
