import { Injectable } from '@angular/core';

interface CoreInfo {
  coreId: string;
  lastNonce: string;
  lastDiff: number;
  highestDiff: number;
  lastDiffMax: number;
  lastTime: string;
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

@Injectable({
  providedIn: 'root'
})
export class PersistenceService {
  private readonly CORE_MAP_KEY = 'miningCoreMap';
  private readonly MINING_TASKS_KEY = 'miningTasks';
  private readonly HIGHEST_DIFF_TASK_KEY = 'highestDiffTask';

  saveCoreMap(coreMap: Record<string, CoreInfo>): void {
    localStorage.setItem(this.CORE_MAP_KEY, JSON.stringify(coreMap));
  }

  loadCoreMap(): Record<string, CoreInfo> {
    const data = localStorage.getItem(this.CORE_MAP_KEY);
    return data ? JSON.parse(data) : {};
  }

  clearCoreMap(): void {
    localStorage.removeItem(this.CORE_MAP_KEY);
  }

  saveMiningTasks(tasks: MiningTask[], highestDiffTask: MiningTask | null): void {
    localStorage.setItem(this.MINING_TASKS_KEY, JSON.stringify(tasks));
    localStorage.setItem(this.HIGHEST_DIFF_TASK_KEY, JSON.stringify(highestDiffTask));
  }

  loadMiningTasks(): { tasks: MiningTask[]; highestDiffTask: MiningTask | null } {
    const tasksData = localStorage.getItem(this.MINING_TASKS_KEY);
    const highestDiffData = localStorage.getItem(this.HIGHEST_DIFF_TASK_KEY);
    return {
      tasks: tasksData ? JSON.parse(tasksData) : [],
      highestDiffTask: highestDiffData ? JSON.parse(highestDiffData) : null
    };
  }

  clearMiningTasks(): void {
    localStorage.removeItem(this.MINING_TASKS_KEY);
    localStorage.removeItem(this.HIGHEST_DIFF_TASK_KEY);
  }
}
