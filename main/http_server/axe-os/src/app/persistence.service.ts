// main/http_server/axe-os/src/app/persistence.service.ts
import { Injectable } from '@angular/core';

interface CoreInfo {
  coreId: string;
  lastNonce: string;
  lastDiff: number;
  highestDiff: number;
  lastDiffMax: number;
  lastTime: string;
}

@Injectable({
  providedIn: 'root'
})
export class PersistenceService {
  private readonly CORE_MAP_KEY = 'miningCoreMap';

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
}
