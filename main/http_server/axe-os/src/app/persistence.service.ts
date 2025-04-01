import { Injectable } from '@angular/core';

// DEV NOTE: Interfaces define the data structures persisted by this service.
// They must match the structures used in MiningMatrixComponent for compatibility.
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
  providedIn: 'root' // DEV NOTE: Service is singleton, available application-wide.
})
export class PersistenceService {
  // DEV NOTE: Constants for localStorage keys to avoid magic strings and ensure consistency.
  private readonly CORE_MAP_KEY = 'miningCoreMap';
  private readonly MINING_TASKS_KEY = 'miningTasks';
  private readonly HIGHEST_DIFF_TASK_KEY = 'highestDiffTask';

  saveCoreMap(coreMap: Record<string, CoreInfo>): void {
    // DEV NOTE: Persists coreMap to localStorage as a JSON string.
    // This ensures core performance data survives screen changes and browser refreshes.
    localStorage.setItem(this.CORE_MAP_KEY, JSON.stringify(coreMap));
  }

  loadCoreMap(): Record<string, CoreInfo> {
    // DEV NOTE: Retrieves coreMap from localStorage, parsing it back to an object.
    // Returns an empty object if no data exists to avoid null checks elsewhere.
    const data = localStorage.getItem(this.CORE_MAP_KEY);
    return data ? JSON.parse(data) : {};
  }

  clearCoreMap(): void {
    // DEV NOTE: Removes coreMap from localStorage, used during system restarts to reset state.
    localStorage.removeItem(this.CORE_MAP_KEY);
  }

  saveMiningTasks(tasks: MiningTask[], highestDiffTask: MiningTask | null): void {
    // DEV NOTE: Saves both the miningTasks array and highestDiffTask separately to localStorage.
    // This allows persistence of the share log across screen changes, with highestDiffTask tracked independently.
    localStorage.setItem(this.MINING_TASKS_KEY, JSON.stringify(tasks));
    localStorage.setItem(this.HIGHEST_DIFF_TASK_KEY, JSON.stringify(highestDiffTask));
  }

  loadMiningTasks(): { tasks: MiningTask[]; highestDiffTask: MiningTask | null } {
    // DEV NOTE: Loads miningTasks and highestDiffTask from localStorage, parsing JSON back to objects.
    // Returns defaults (empty array and null) if no data exists, ensuring safe initialization.
    const tasksData = localStorage.getItem(this.MINING_TASKS_KEY);
    const highestDiffData = localStorage.getItem(this.HIGHEST_DIFF_TASK_KEY);
    return {
      tasks: tasksData ? JSON.parse(tasksData) : [],
      highestDiffTask: highestDiffData ? JSON.parse(highestDiffData) : null
    };
  }

  clearMiningTasks(): void {
    // DEV NOTE: Clears miningTasks and highestDiffTask from localStorage, triggered on system restart.
    // Ensures the share log resets when the system restarts, as per requirements.
    localStorage.removeItem(this.MINING_TASKS_KEY);
    localStorage.removeItem(this.HIGHEST_DIFF_TASK_KEY);
  }
}
