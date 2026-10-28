# asterism JavaScript client

Starts `asterism mcp` and gives it tasks. Node.js 22+, TypeScript types
included, no other dependencies.

```javascript
import { Client } from '@asterism/sdk';

const client = await Client.start(['asterism', 'mcp']);
const task = await client.session('example').submit('Hello');
const result = await task.wait({ timeout: 120_000 });
console.log(result.answer);
await task.release();
await client.close();
```

Times are in milliseconds. `done` means the task ended, not that it worked:
check `task_state`. Giving up on a wait does not stop the task: call
`task.cancel()`. More in `sdk/README.md` of the asterism repository.
