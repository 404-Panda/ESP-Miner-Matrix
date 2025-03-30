// main/http_server/axe-os/src/app/components/mining-matrix/mining-matrix.component.ts
import { Component, OnDestroy, OnInit, ElementRef, ViewChild } from '@angular/core';
import { PersistenceService } from '../../persistence.service';

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
  midstate: string;
  header: string;
  nonce: string;
  difficulty: number;
  timestamp: number;
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
  private lastJob: { jobId: string; version: string; coreId: string } | null = null;

  public totalCores = 896;
  public gridSize = Math.ceil(Math.sqrt(this.totalCores));
  public cores: { big: number; small: number }[] = [];

  public shareScatter: ScatterPoint[] = [];

  public chartData: any;
  public chartOptions: any;

  @ViewChild('scrollContainer', { static: false })
  private scrollContainer?: ElementRef<HTMLDivElement>;

  constructor(private persistenceService: PersistenceService) {}

  ngOnInit(): void {
    this.coreMap = this.persistenceService.loadCoreMap();
    this.updateTopCores();
    this.recalculateBestCore();

    for (let b = 0; b < 112; b++) {
      for (let s = 0; s < 8; s++) {
        this.cores.push({ big: b, small: s });
      }
    }

    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const wsUrl = `${protocol}://${window.location.host}/api/ws`;
    this.ws = new WebSocket(wsUrl);

    this.ws.onopen = () => {
      console.log('WebSocket connected');
    };
    this.ws.onmessage = (event) => {
      const text = event.data as string;
      console.log('WebSocket message:', text);
      this.handleIncomingLog(text);
    };
    this.ws.onerror = (error) => {
      console.error('WebSocket error:', error);
    };
    this.ws.onclose = () => {
      console.log('WebSocket closed');
    };

    this.chartData = {
      datasets: [
        {
          label: 'Difficulty',
          data: this.shareScatter,
          showLine: false,
          fill: false,
          pointRadius: (context: any) => {
            const point = this.shareScatter[context.dataIndex];
            return point ? point.radius : 3;
          },
          backgroundColor: (context: any) => {
            const point = this.shareScatter[context.dataIndex];
            return point ? point.color : '#42A5F5';
          }
        }
      ]
    };

    this.chartOptions = {
      responsive: true,
      maintainAspectRatio: false,
      scales: {
        x: {
          type: 'time',
          time: { unit: 'second' },
          title: { display: true, text: 'Time' }
        },
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
    if (this.ws) this.ws.close();
    this.persistenceService.saveCoreMap(this.coreMap);
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

    this.updateCoreInfo(parsed);
    this.updateMiningTasks(parsed);

    if (this.bestCoreId && parsed.core === this.bestCoreId) {
      this.bestCoreDetailLines.push(rawLine);
      if (this.bestCoreDetailLines.length > 50) this.bestCoreDetailLines.shift();
    }

    setTimeout(() => this.scrollToBottom(), 20);
  }

  private updateMiningTasks(parsed: ParsedLine): void {
    if (parsed.category === 'jobInfo') {
      this.lastJob = {
        jobId: parsed.jobId,
        version: parsed.version,
        coreId: parsed.core
      };
    } else if (parsed.category === 'asic_result' && this.lastJob) {
      const task: MiningTask = {
        jobId: this.lastJob.jobId,
        coreId: this.lastJob.coreId,
        version: this.lastJob.version,
        midstate: this.generateMockMidstate(),
        header: this.generateMockHeader(this.lastJob.version, parsed.nonce),
        nonce: parsed.nonce,
        difficulty: parsed.diff,
        timestamp: Date.now()
      };

      this.miningTasks.unshift(task);
      if (this.miningTasks.length > 5) this.miningTasks.pop();
    }
  }

  private generateMockMidstate(): string {
    return Array(64).fill('0').join('') + Math.random().toString(16).slice(2, 10).padEnd(32, '0');
  }

  private generateMockHeader(version: string, nonce: string): string {
    const prevBlockHash = Array(64).fill('0').join('');
    const merkleRoot = Array(64).fill('0').join('');
    const ntime = Math.floor(Date.now() / 1000).toString(16).padStart(8, '0');
    const target = '00000000ffff0000';
    return `${version}${prevBlockHash}${merkleRoot}${ntime}${target}${nonce}`.slice(0, 160);
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

  private getColorForDiff(diff: number, isBest: boolean): string {
    let color = '';
    if (diff <= 200) color = '#4B0000';
    else if (diff <= 500) color = '#8B0000';
    else if (diff <= 1000) color = '#A30000';
    else if (diff <= 5000) color = '#CC0000';
    else if (diff <= 10000) color = '#FF3333';
    else if (diff <= 50000) color = '#006600';
    else if (diff <= 100000) color = '#008000';
    else if (diff <= 500000) color = '#00B300';
    else if (diff <= 1000000) color = '#33FF33';
    else if (diff <= 5000000) color = '#CCCC00';
    else if (diff <= 10000000) color = '#E6E600';
    else if (diff <= 50000000) color = '#FFFF00';
    else if (diff <= 100000000) color = '#FFFF66';
    else if (diff <= 500000000) color = '#FF9900';
    else if (diff <= 1000000000) color = '#FFAD33';
    else if (diff <= 10000000000) color = '#FFC107';
    else color = '#FFCC66';
    return color;
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
          highestDiff: highestDiff,
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

  public formatHex(hex: string, maxLength: number = 16): string {
    return hex.length > maxLength ? `${hex.slice(0, maxLength / 2)}...${hex.slice(-maxLength / 2)}` : hex;
  }

  public trackByJobId(index: number, task: MiningTask): string {
    return `${task.jobId}-${task.coreId}-${task.timestamp}`;
  }

  public isNewTask(task: MiningTask): boolean {
    return (Date.now() - task.timestamp) < 2000;
  }
}
