// The one icon Fluent's set does not have: Windows' shield for "this asks for administrator approval".
export function UacShield() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style={{ flex: 'none' }}>
      <path d="M8 1 2.5 3.2v4.3C2.5 11 4.8 13.6 8 15c3.2-1.4 5.5-4 5.5-7.5V3.2z" fill="#4c8dff" />
      <path d="M8 1v7H2.5v-.5V3.2zM8 8h5.5c0 3.5-2.3 6.1-5.5 7.5z" fill="#f5c542" />
    </svg>
  );
}
