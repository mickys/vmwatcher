import fs from "node:fs/promises";
import path from "node:path";
import { uuidPattern } from "./libvirt.mjs";
// Named sets of VMs for the wall, shared by every browser that opens it.
// Stored as one small JSON file in the viewer's data directory.
const LIMIT = 50,
  MEMBERS = 100;
export function checkGroup(name, vms) {
  if (typeof name !== "string") throw Error("Invalid group name");
  name = name.trim();
  if (!name || name.length > 60 || /[\u0000-\u001f]/.test(name))
    throw Error("Group names are 1 to 60 characters");
  if (
    !Array.isArray(vms) ||
    vms.length > MEMBERS ||
    !vms.every((id) => typeof id === "string" && uuidPattern.test(id))
  )
    throw Error("Invalid group members");
  return { name, vms: [...new Set(vms)] };
}
export class Groups {
  constructor(root) {
    this.file = path.join(root, "groups.json");
    this.queue = Promise.resolve();
  }
  async list() {
    try {
      const data = JSON.parse(await fs.readFile(this.file, "utf8"));
      return Array.isArray(data) ? data : [];
    } catch (error) {
      if (error.code === "ENOENT") return [];
      throw error;
    }
  }
  // Create the group, or replace its members when the name already exists.
  save(name, vms) {
    const group = checkGroup(name, vms);
    const job = this.queue.then(async () => {
      const groups = await this.list();
      const at = groups.findIndex((g) => g.name === group.name);
      if (at >= 0) groups[at] = group;
      else if (groups.length >= LIMIT) throw Error("Too many groups");
      else groups.push(group);
      groups.sort((a, b) => a.name.localeCompare(b.name));
      await fs.mkdir(path.dirname(this.file), { recursive: true });
      const temporary = this.file + ".tmp";
      await fs.writeFile(temporary, JSON.stringify(groups, null, 2), {
        mode: 0o600,
      });
      await fs.rename(temporary, this.file);
      return groups;
    });
    this.queue = job.catch(() => {});
    return job;
  }
}
