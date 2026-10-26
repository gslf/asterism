# SDKs

Python (`asterism-sdk`, import `asterism`) and JavaScript/TypeScript
(`@asterism/sdk`) clients for `asngn-mcp`. Each client starts the server with
an explicit argv and talks MCP over stdio. No runtime dependencies: Python
3.11+ or Node.js 22+.

```bash
python3 -m pip install ./sdk/python
npm install ./sdk/javascript
```

```python
import asyncio
from asterism import Client

async def main():
    async with await Client.start(["asngn-mcp", "--root", "/abs/engine-root"]) as client:
        task = await client.session("example").submit("Find the missing tests")
        async for page in task.updates(timeout=120):
            for event in page["events"]:
                print(event["text"], end="")
            if page["done"]:
                print(page["outcome"], page["task_state"])
        await task.release()

asyncio.run(main())
```

```typescript
import { Client } from '@asterism/sdk';

const client = await Client.start(['asngn-mcp', '--root', '/abs/engine-root']);
try {
  const task = await client.session('example').submit('Find the missing tests');
  const result = await task.wait({ timeout: 120_000 });
  console.log(result.outcome, result.task_state, result.answer);
  await task.release();
} finally {
  await client.close();
}
```

Durations are seconds in Python and milliseconds in JavaScript.

## Rules worth knowing

- `done` means the task ended, not that it succeeded. Success is
  `task_state: "succeeded"`, which needs the host's acceptance criteria
  (`define_work` / `defineWork`) and runtime evidence.
- A local timeout or cancelled wait stops waiting only. `task.cancel()` cancels
  the engine task; poll until `done`, then `release()`.
- A `submit` whose reply was lost may have run: never retry it blindly.
  `session.recover(task_id)` reads the durable outcome after a release or a
  server restart; it never re-executes.
- `approval()` is read-only. Approvals are granted by trusted hosts through the
  C API, never by an MCP client.
- `consumption()` returns engine-wide inference accounting; known tokens,
  unknown usage and open reservations are kept separate.
- Frames and requests are capped at 8 MiB, with at most 64 pending requests.
  Malformed or ambiguous wire data fails closed.

## Tests

CMake registers the SDK checks (`ctest -R '^sdk\.'`) when Python and Node are
available. `npm ci --prefix sdk --ignore-scripts` adds the TypeScript check.
`python sdk/tests/package_smoke.py <asngn-mcp> <node> <tsc>` builds, installs
and exercises both packages away from the checkout.
