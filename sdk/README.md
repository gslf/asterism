# Python and JavaScript clients

Small libraries that start `asterism mcp` and talk to it, so your own programs
can give tasks to the agent. They need nothing else: Python 3.11+ or Node.js 22+.

```bash
python3 -m pip install ./sdk/python
npm install ./sdk/javascript
```

```python
import asyncio
from asterism import Client

async def main():
    async with await Client.start(["asterism", "mcp"]) as client:
        task = await client.session("example").submit("Find the missing tests")
        result = await task.wait(timeout=120)
        print(result["answer"])
        await task.release()

asyncio.run(main())
```

```typescript
import { Client } from '@asterism/sdk';

const client = await Client.start(['asterism', 'mcp']);
const task = await client.session('example').submit('Find the missing tests');
console.log((await task.wait({ timeout: 120_000 })).answer);
await task.release();
await client.close();
```

Good to know:

- `done` means the task ended, not that it worked: check `task_state`.
- Giving up on a wait does not stop the task: call `task.cancel()`.
- If the reply to `submit` got lost, do not send it again:
  `session.recover(task_id)` tells you what happened.
- Times are in seconds in Python and in milliseconds in JavaScript.

`ctest --test-dir build -R '^sdk\.'` runs the client tests.
