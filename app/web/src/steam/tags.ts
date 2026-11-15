// The Workshop's tags: one list in English, steam/tags.json, which filtering here, publishing and
// the tags set up for the Workshop in Steamworks all use (steam/PROTOCOL.md "Tags"). The page shows
// each in its own language (steam.tag.<the name in lowercase, spaces as underscores> in
// res/locales); a tag not in the list is shown as it is.
import vocabulary from '../../../../steam/tags.json';
import { t, type Key } from '../i18n';

export const TAGS: { type: string[]; resolution: string[]; content: string[]; rating: string[] } = vocabulary;

const KNOWN = new Set([...TAGS.type, ...TAGS.resolution, ...TAGS.content, ...TAGS.rating]);

export const tagLabel = (tag: string) => (KNOWN.has(tag) ? t(`steam.tag.${tag.toLowerCase().replace(/ /g, '_')}` as Key) : tag);

// An item's age rating as a badge on its picture: only when it is not for everyone.
export const ratingBadge = (rating: string | undefined) => (rating === 'Questionable' || rating === 'Mature' ? tagLabel(rating) : null);

// A content descriptor (what an author says an item contains, steam/PROTOCOL.md "Names").
const DESCRIPTORS = new Set(['nuditySexual', 'violenceGore', 'adultOnlySexual', 'gratuitousSexual', 'generalMature']);
export const descriptorLabel = (d: string) => (DESCRIPTORS.has(d) ? t(`steam.descriptor.${d}` as Key) : t('steam.descriptor.other'));
