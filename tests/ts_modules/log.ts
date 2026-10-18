// evaluation log of the test modules
export const entries: string[] = [];
export function log(name: string): void {
    entries.push(name);
}
