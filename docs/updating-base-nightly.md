# How to Update the Base Nightly Version

Your fork builds a patched version of UEVR on top of a specific **praydog nightly release**. The file `BASE_NIGHTLY` in the repo root tracks which nightly your fork is based on.

## When to update

Update `BASE_NIGHTLY` whenever you **pull new changes from upstream** (praydog's repo), so that your release page tells users to download the correct matching nightly.

## Steps

### 1. Pull upstream changes

```bash
git fetch upstream
git merge upstream/master
```

### 2. Find the new nightly tag

Go to [praydog/UEVR-nightly releases](https://github.com/praydog/UEVR-nightly/releases) and find the release whose commit SHA matches the latest upstream commit. The tag looks like:

```
nightly-01127-6f66affc01cea22e4b1b5a47986e1ade80ccbd26
```

You can see the upstream HEAD commit with:

```bash
git log upstream/master -1 --format="%H"
```

Then search for that SHA in the nightly release tags.

### 3. Update the file

Edit `BASE_NIGHTLY` and replace the tag with the new one:

```
nightly-XXXXX-<full commit sha>
```

Just one line, no extra spaces.

### 4. Commit and push

```bash
git add BASE_NIGHTLY
git commit -m "Update base nightly to XXXXX"
git push
```

The next release will automatically reference the updated nightly on the release page.

## What the release page shows

Each release will include instructions like:

> 1. Download the base UEVR nightly: **[nightly-01127-6f66aff...](https://github.com/praydog/UEVR-nightly/releases/tag/...)**
> 2. Extract the nightly zip
> 3. Download **uevr.zip** from this release
> 4. Overwrite/replace the nightly files with the ones from this release
> 5. Run UEVR as normal

Plus a list of changes (commit messages) since the last release.
