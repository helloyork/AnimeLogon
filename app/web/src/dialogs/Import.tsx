// What to import, asked before anything is opened: a video, a picture, a theme package, or Wallpaper
// Engine's wallpapers. Each kind says what it makes and which file types it takes; the types are
// the ones the native side's file dialog lists for that kind (environment `importable`).
// Wallpaper Engine stays in the list where it is not found, unavailable, saying why.
import { useId, type ReactNode } from 'react';
import { Dialog, DialogActions, DialogBody, DialogContent, DialogSurface, DialogTitle } from '@fluentui/react-components';
import { Box20Regular, Image20Regular, Video20Regular, VideoClipMultiple20Regular } from '@fluentui/react-icons';
import { list, t as tr } from '../i18n';
import { UacShield } from '../icons';
import { closeDialog, importKind, useStore, type Dialog as DialogState } from '../store';
import type { ImportKind } from '../types';
import { Button, cx } from '../ui/controls';

const ICONS: Record<ImportKind, ReactNode> = { video: <Video20Regular />, picture: <Image20Regular />, package: <Box20Regular /> };
const KINDS: ImportKind[] = ['video', 'picture', 'package'];

export function ImportDialog({ d }: { d: Extract<DialogState, { kind: 'import' }> }) {
  const s = useStore();
  const close = () => closeDialog(d.id);
  const choose = (kind: ImportKind | 'workshop') => {
    close();
    importKind(kind, d.from);
  };
  const we = s.env.wallpaperEngine;
  return (
    <Dialog open onOpenChange={(_, data) => !data.open && close()}>
      <DialogSurface className="dialog" style={{ maxWidth: '480px' }}>
        <DialogBody>
          <DialogTitle>{tr('dialog.import.title')}</DialogTitle>
          <DialogContent>
            <div className="kinds">
              {KINDS.map(k => (
                // A file dialog follows, then the administrator's approval to install what is chosen.
                <Kind key={k} icon={ICONS[k]} name={tr(`dialog.import.${k}`)} shield detail={tr(`dialog.import.${k}.detail`)}
                  types={list(s.env.importable[k].map(e => '.' + e))} onChoose={() => choose(k)} />
              ))}
              {/* Its own dialog follows, whose Import asks for the approval. */}
              <Kind icon={<VideoClipMultiple20Regular />} name="Wallpaper Engine" detail={tr(we ? 'dialog.import.we.detail' : 'dialog.import.we.missing')}
                types={tr('dialog.import.we.types')} unavailable={!we} onChoose={() => choose('workshop')} />
            </div>
          </DialogContent>
          <DialogActions>
            <Button onClick={close}>{tr('button.cancel')}</Button>
          </DialogActions>
        </DialogBody>
      </DialogSurface>
    </Dialog>
  );
}

// One kind: a button named by its name and described by what it makes and the types it takes.
// An unavailable one can still be reached by the keyboard, so its reason is read out, but does nothing.
function Kind({ icon, name, shield, detail, types, unavailable, onChoose }: {
  icon: ReactNode;
  name: string;
  shield?: boolean;
  detail: string;
  types: string;
  unavailable?: boolean;
  onChoose: () => void;
}) {
  const id = useId();
  return (
    <button type="button" className={cx('kind', unavailable && 'off')} aria-labelledby={id + 'n'} aria-describedby={`${id}d ${id}t`}
      aria-disabled={unavailable || undefined} onClick={() => !unavailable && onChoose()}>
      <span className="kind-icon" aria-hidden="true">{icon}</span>
      <span className="kind-text">
        <span className="kind-name" id={id + 'n'}>{shield && <UacShield />}{name}</span>
        <span className="t-cap" id={id + 'd'}>{detail}</span>
        <span className="t-cap3" id={id + 't'}>{types}</span>
      </span>
    </button>
  );
}
