import type { MouseEvent, ReactNode } from 'react'
import { navigateTo } from '../routing'

export function AppLink({
  href,
  className,
  children,
  ariaCurrent,
}: {
  href: string
  className?: string
  children: ReactNode
  ariaCurrent?: 'page'
}) {
  function handleClick(event: MouseEvent<HTMLAnchorElement>) {
    if (
      event.defaultPrevented ||
      event.button !== 0 ||
      event.metaKey ||
      event.ctrlKey ||
      event.shiftKey ||
      event.altKey
    ) {
      return
    }
    event.preventDefault()
    navigateTo(href)
  }

  return (
    <a
      href={href}
      className={className}
      aria-current={ariaCurrent}
      onClick={handleClick}
    >
      {children}
    </a>
  )
}
