# asterism Python client

Starts `asterism mcp` and gives it tasks. Python 3.11+, no other dependencies.

```python
from asterism import Client

async def run():
    async with await Client.start(["asterism", "mcp"]) as client:
        task = await client.session("example").submit("Hello")
        result = await task.wait(timeout=120)
        print(result["answer"])
        await task.release()
```

Times are in seconds. `done` means the task ended, not that it worked: check
`task_state`. Giving up on a wait does not stop the task: call `task.cancel()`.
More in `sdk/README.md` of the asterism repository.
