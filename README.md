## Restore repository to existing folder (e.g. keep build folder)

If you have an existing folder (e.g. with build artifacts) and want to restore the repository content:

```powershell
git init
git remote add origin https://github.com/bluetapok-create/finder.git
git fetch
git checkout -t origin/main -f
```
